"""Documents resource. ``/documents`` domain (ARCH § 4.1.2 + F42).

Per Derek's ruling (briefing § 9): SDK shape = P03 § 2.12; wire = real
architecture. ``upload()`` is **async** (aligns with the F42 Document Async
Processing being built this round — § 2.12's old sync upload is obsoleted by
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

from .._exceptions import CortrixError, InvalidRequestError
from ..types import Document, DocumentTask
from ..types.lists import DocumentList
from ._base import AsyncResource, SyncResource

# --- paths (real-arch wire; centralised) ---
PATH_DOCUMENTS = "/documents"
PATH_DOCUMENT = "/documents/{id}"
PATH_TASK_PROGRESS = "/documents/tasks/{task_id}/progress"
PATH_TASK = "/documents/tasks/{task_id}"

# Terminal async-task states (F42 DocumentTask.status enum).
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
