"""Documents resource. ``/documents`` domain (ARCH § 4.1.2 + async task).

This resource follows the implemented HTTP architecture. ``upload()`` is
**async** (aligns with the async task Document Async Processing path; the old sync upload is superseded by
Async task): ``POST /documents`` -> 202 ``DocumentTask``, with
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

from .._exceptions import CortrixError, InvalidRequestError
from ..types import Document, DocumentTask
from ..types.lists import DocumentList
from ._base import AsyncResource, SyncResource

# --- paths (real-arch wire; centralized) ---
PATH_DOCUMENTS = "/documents"
PATH_DOCUMENT = "/documents/{id}"
PATH_TASK_PROGRESS = "/documents/tasks/{task_id}/progress"
PATH_TASK = "/documents/tasks/{task_id}"
# Agent-first batch submit. The design (Python SDK / batch submit §2.1)
# names this ``/documents/batch-submit``; the real-arch wire is ``/documents/batch``
# (server route POST /api/v1/documents/batch, RegisterBatchRoutes), so per the
# "SDK shape follows the SDK design, wire follows the real architecture" rule we map to the live route.
PATH_BATCH = "/documents/batch"

# on_duplicate policy values (batch submit §2.2 options.on_duplicate).
_ON_DUPLICATE_VALUES = frozenset({"skip", "overwrite", "error"})

# Terminal async-task states (async task DocumentTask.status enum).
_TERMINAL_TASK_STATES = frozenset({"ready", "failed", "cancelled"})
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
    """Build the batch-submit request body (batch submit §2.2).

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
        """Upload a document (async). ``POST /documents`` -> 202 DocumentTask.

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

        Returns the final ``DocumentTask`` (status ready/failed/cancelled).
        Raises ``TimeoutError`` if ``timeout`` elapses first.
        """
        task = self.upload(namespace, file, filename=filename, metadata=metadata)
        deadline = time.monotonic() + timeout
        while task.status not in _TERMINAL_TASK_STATES:
            if time.monotonic() >= deadline:
                raise CortrixError(
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
        """Batch-submit up to 100 documents (batch submit). ``POST /documents/batch``.

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
        """Async sugar: upload then poll until a terminal state (ready/failed/cancelled)."""
        task = await self.upload(namespace, file, filename=filename, metadata=metadata)
        deadline = time.monotonic() + timeout
        while task.status not in _TERMINAL_TASK_STATES:
            if time.monotonic() >= deadline:
                raise CortrixError(
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
        """Batch-submit up to 100 documents (batch submit). ``POST /documents/batch``.

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
