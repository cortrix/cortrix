"""Documents resource. ``/documents`` domain (ARCH § 4.1.2 + F42).

This resource follows the implemented HTTP architecture. ``upload()`` is
**async** (aligns with the F42 Document Async Processing path; the old sync upload is superseded by
F42): ``POST /documents`` -> 202 ``DocumentTask``, with
``GET /documents/tasks/{task_id}/progress`` + ``DELETE /documents/tasks/{task_id}``.
``upload_and_wait()`` is sync sugar (upload then poll to a terminal state).
``status()`` aliases ``get()`` (status lives in ``Document.status`` / ``progress``).
Real multipart large-file endpoint -> D3.5.
"""

from __future__ import annotations

import asyncio
import base64
import time
from pathlib import Path
from typing import Any, BinaryIO, Optional, Union

from .._exceptions import InvalidRequestError
# Aliased on purpose: `TimeoutError` unqualified would shadow the builtin inside
# this module. The SDK class subclasses CortrixError, so raising it keeps every
# existing `except CortrixError` handler working.
from .._exceptions import TimeoutError as CortrixTimeoutError
from ..types import Document, DocumentTask
from ..types.lists import DocumentList
from ._base import AsyncResource, SyncResource

# --- paths (real-arch wire; centralized) ---
PATH_DOCUMENTS = "/documents"
PATH_DOCUMENT = "/documents/{id}"
PATH_TASK_PROGRESS = "/documents/tasks/{task_id}/progress"
PATH_TASK = "/documents/tasks/{task_id}"
# TD-F42-BULK Agent-first batch submit. The design (P03 §2.12 / TD-F42-BULK §2.1)
# names this ``/documents/batch-submit``; the real-arch wire is ``/documents/batch``
# (server route POST /api/v1/documents/batch, RegisterBatchRoutes), so per the
# "SDK shape = P03, wire = real architecture" rule we map to the live route.
PATH_BATCH = "/documents/batch"

# on_duplicate policy values (TD-F42-BULK §2.2 options.on_duplicate).
_ON_DUPLICATE_VALUES = frozenset({"skip", "overwrite", "error"})

# Terminal async-task states, i.e. the ones that end a wait.
#
# Authority is the server's own state machine (include/cortrix/async/task_info.h),
# which the tasks table pins with a CHECK constraint -- not the OpenAPI enum, which
# named a value the server has never emitted and omitted the only terminal success
# state. `completed` is that success state.
#
# `cancelling` is deliberately NOT here: it is the transient state a RUNNING task
# enters when cancellation is requested, so a wait must keep going until it resolves.
# What it resolves to is not fixed -- a late cancel can still land on `completed` or
# `failed`. (A task cancelled before it starts never passes through `cancelling` at
# all; it goes straight to `cancelled` -- src/async/task_manager.cpp:629-632.)
#
# `ready` was previously in this set. It is removed rather than kept as tolerance
# because no server version has ever emitted it on a task: it belongs to
# Document.status, a different state machine. Keeping a dead member here is what let
# the declaration and the runtime disagree in the first place.
_TERMINAL_TASK_STATES = frozenset({"completed", "failed", "cancelled"})
_DEFAULT_POLL_INTERVAL = 1.0
_DEFAULT_WAIT_TIMEOUT = 300.0


def _read_content(
    file: Union[str, Path, BinaryIO], filename: Optional[str]
) -> tuple[str, str]:
    """Return ``(content, filename)``.

    Text files are sent verbatim; binary content is base64-encoded (the spec's
    ``content`` field accepts text or base64). A path supplies its basename when
    ``filename`` is omitted; a file-like object requires an explicit filename.
    """
    if isinstance(file, (str, Path)):
        p = Path(file)
        raw = p.read_bytes()
        resolved = filename or p.name
    else:
        raw = file.read()
        if filename is None:
            raise InvalidRequestError(
                "filename is required when uploading a file-like object",
                category="permanent",
                retryable=False,
            )
        resolved = filename
        if isinstance(raw, str):
            raw = raw.encode("utf-8")
    try:
        content = raw.decode("utf-8")
    except UnicodeDecodeError:
        content = base64.b64encode(raw).decode("ascii")
    return content, resolved


def _upload_body(
    namespace: str, content: str, filename: str, metadata: Optional[dict[str, Any]]
) -> dict[str, Any]:
    body: dict[str, Any] = {"namespace": namespace, "content": content, "filename": filename}
    if metadata is not None:
        body["metadata"] = metadata
    return body


def _list_params(namespace: str, limit: int, offset: int) -> dict[str, Any]:
    return {"namespace": namespace, "limit": limit, "offset": offset}


def _batch_body(
    namespace: str,
    documents: list[dict[str, Any]],
    async_: bool,
    on_duplicate: str,
) -> dict[str, Any]:
    """Build the batch-submit request body (TD-F42-BULK §2.2).

    ``documents`` items are passed through verbatim (each needs a ``doc_id`` +
    ``content``; optional ``filename`` / ``metadata``) so the caller controls the
    client-supplied ``doc_id``. ``on_duplicate`` is validated client-side to fail
    fast before the round-trip.
    """
    if on_duplicate not in _ON_DUPLICATE_VALUES:
        raise InvalidRequestError(
            f"on_duplicate must be one of {sorted(_ON_DUPLICATE_VALUES)}",
            category="permanent",
            retryable=False,
        )
    return {
        "namespace": namespace,
        "documents": documents,
        "options": {"async": async_, "on_duplicate": on_duplicate},
    }


class Documents(SyncResource):
    """Document operations. ``/documents`` domain."""

    def upload(
        self,
        namespace: str,
        file: Union[str, Path, BinaryIO],
        *,
        filename: Optional[str] = None,
        metadata: Optional[dict[str, Any]] = None,
    ) -> DocumentTask:
        """Upload a document (async, F42). ``POST /documents`` -> 202 DocumentTask.

        Poll progress via :meth:`task_progress`, or use :meth:`upload_and_wait`.
        """
        content, resolved = _read_content(file, filename)
        return self._client._request(
            "POST",
            PATH_DOCUMENTS,
            json=_upload_body(namespace, content, resolved, metadata),
            response_model=DocumentTask,
        )

    def upload_and_wait(
        self,
        namespace: str,
        file: Union[str, Path, BinaryIO],
        *,
        filename: Optional[str] = None,
        metadata: Optional[dict[str, Any]] = None,
        poll_interval: float = _DEFAULT_POLL_INTERVAL,
        timeout: float = _DEFAULT_WAIT_TIMEOUT,
    ) -> DocumentTask:
        """Sync sugar: upload then poll until the task reaches a terminal state.

        Returns the final ``DocumentTask``. A finished upload arrives as
        ``completed``; the other terminal states are ``failed`` and ``cancelled``.
        ``cancelling`` does not end the wait. It is the window after cancelling a
        task that is already running; a task cancelled before it starts goes straight
        to ``cancelled``, and a cancel that arrives late can still finish as
        ``completed`` or ``failed``.

        Raises ``cortrix.TimeoutError`` if ``timeout`` elapses first. It subclasses
        ``CortrixError`` and carries ``category="timeout"`` and ``retryable=True``,
        so handlers catching either one still work.
        """
        task = self.upload(namespace, file, filename=filename, metadata=metadata)
        deadline = time.monotonic() + timeout
        while task.status not in _TERMINAL_TASK_STATES:
            if time.monotonic() >= deadline:
                raise CortrixTimeoutError(
                    f"upload task {task.task_id} did not finish within {timeout}s",
                    category="timeout",
                    retryable=True,
                )
            time.sleep(poll_interval)
            task = self.task_progress(task.task_id)
        return task

    def batch_submit(
        self,
        namespace: str,
        documents: list[dict[str, Any]],
        *,
        async_: bool = True,
        on_duplicate: str = "skip",
    ) -> dict[str, Any]:
        """Batch-submit up to 100 documents (TD-F42-BULK). ``POST /documents/batch``.

        Agent-first bulk upload: one request submits many documents instead of N
        single ``upload()`` calls. Each ``documents`` item is a dict with a
        client-supplied ``doc_id`` + ``content`` (optionally ``filename`` /
        ``metadata``).

        ``async_`` must stay ``True`` in V1 (synchronous batch is unsupported).
        ``on_duplicate`` is one of ``skip`` / ``overwrite`` / ``error``.

        Returns the raw partial-success envelope (``results`` + ``meta`` with
        ``succeeded`` / ``failed[]`` GEN-Agent 5-field entries / ``coverage_ratio``
        / ``total_submitted``) so the Agent can branch per-doc on retryable
        failures. Batch-level faults (size/payload/empty/duplicate-doc_id) raise
        the standard ``CortrixError`` envelope.
        """
        return self._client._request(
            "POST",
            PATH_BATCH,
            json=_batch_body(namespace, documents, async_, on_duplicate),
            response_model=None,
        )

    def list(
        self, namespace: str, *, limit: int = 50, offset: int = 0
    ) -> DocumentList:
        """List documents. ``GET /documents?namespace=&limit=&offset=``."""
        return self._client._request(
            "GET",
            PATH_DOCUMENTS,
            params=_list_params(namespace, limit, offset),
            response_model=DocumentList,
        )

    def get(self, document_id: str) -> Document:
        """Get document detail. ``GET /documents/{id}``."""
        return self._client._request(
            "GET", PATH_DOCUMENT.format(id=document_id), response_model=Document
        )

    def status(self, document_id: str) -> Document:
        """Document status — alias of :meth:`get` (status lives in the document's
        ``status`` / ``progress`` fields, real-arch wire has no ``/status`` subpath)."""
        return self.get(document_id)

    def task_progress(self, task_id: str) -> DocumentTask:
        """Async task progress. ``GET /documents/tasks/{task_id}/progress``."""
        return self._client._request(
            "GET", PATH_TASK_PROGRESS.format(task_id=task_id), response_model=DocumentTask
        )

    def cancel_task(self, task_id: str) -> DocumentTask:
        """Cancel an async task. ``DELETE /documents/tasks/{task_id}``."""
        return self._client._request(
            "DELETE", PATH_TASK.format(task_id=task_id), response_model=DocumentTask
        )

    def delete(self, document_id: str) -> None:
        """Delete a document and its blocks. ``DELETE /documents/{id}`` -> 204."""
        self._client._request("DELETE", PATH_DOCUMENT.format(id=document_id))


class AsyncDocuments(AsyncResource):
    """Async Documents (symmetric to :class:`Documents`)."""

    async def upload(
        self,
        namespace: str,
        file: Union[str, Path, BinaryIO],
        *,
        filename: Optional[str] = None,
        metadata: Optional[dict[str, Any]] = None,
    ) -> DocumentTask:
        content, resolved = _read_content(file, filename)
        return await self._client._request(
            "POST",
            PATH_DOCUMENTS,
            json=_upload_body(namespace, content, resolved, metadata),
            response_model=DocumentTask,
        )

    async def upload_and_wait(
        self,
        namespace: str,
        file: Union[str, Path, BinaryIO],
        *,
        filename: Optional[str] = None,
        metadata: Optional[dict[str, Any]] = None,
        poll_interval: float = _DEFAULT_POLL_INTERVAL,
        timeout: float = _DEFAULT_WAIT_TIMEOUT,
    ) -> DocumentTask:
        """Async sugar: upload then poll until a terminal state.

        Terminal states are ``completed``, ``failed`` and ``cancelled``; see the
        sync version. Raises ``cortrix.TimeoutError`` on timeout, as it does.
        """
        task = await self.upload(namespace, file, filename=filename, metadata=metadata)
        deadline = time.monotonic() + timeout
        while task.status not in _TERMINAL_TASK_STATES:
            if time.monotonic() >= deadline:
                raise CortrixTimeoutError(
                    f"upload task {task.task_id} did not finish within {timeout}s",
                    category="timeout",
                    retryable=True,
                )
            await asyncio.sleep(poll_interval)
            task = await self.task_progress(task.task_id)
        return task

    async def batch_submit(
        self,
        namespace: str,
        documents: list[dict[str, Any]],
        *,
        async_: bool = True,
        on_duplicate: str = "skip",
    ) -> dict[str, Any]:
        """Batch-submit up to 100 documents (TD-F42-BULK). ``POST /documents/batch``.

        Async-symmetric to :meth:`Documents.batch_submit`. Returns the raw
        partial-success envelope (``results`` + ``meta``).
        """
        return await self._client._request(
            "POST",
            PATH_BATCH,
            json=_batch_body(namespace, documents, async_, on_duplicate),
            response_model=None,
        )

    async def list(
        self, namespace: str, *, limit: int = 50, offset: int = 0
    ) -> DocumentList:
        return await self._client._request(
            "GET",
            PATH_DOCUMENTS,
            params=_list_params(namespace, limit, offset),
            response_model=DocumentList,
        )

    async def get(self, document_id: str) -> Document:
        return await self._client._request(
            "GET", PATH_DOCUMENT.format(id=document_id), response_model=Document
        )

    async def status(self, document_id: str) -> Document:
        return await self.get(document_id)

    async def task_progress(self, task_id: str) -> DocumentTask:
        return await self._client._request(
            "GET", PATH_TASK_PROGRESS.format(task_id=task_id), response_model=DocumentTask
        )

    async def cancel_task(self, task_id: str) -> DocumentTask:
        return await self._client._request(
            "DELETE", PATH_TASK.format(task_id=task_id), response_model=DocumentTask
        )

    async def delete(self, document_id: str) -> None:
        await self._client._request("DELETE", PATH_DOCUMENT.format(id=document_id))
