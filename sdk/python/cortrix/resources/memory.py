"""Memory resource. ``/memory`` domain (ARCH § 4.1.5 + MEM01-05).

Per Derek's ruling (briefing § 9): SDK shape = P03 § 2.12; wire = real
architecture. Method -> endpoint:
  - ``search``   -> POST   /memory/search   ({memories})
  - ``log``      -> POST   /memory/sessions/{id}/interactions  (live wire)
  - ``extract``  -> POST   /memory/extract  (real-arch, MEM02; PartialSuccessById)
  - ``list``     -> GET    /memory          ({memories,total,...})
  - ``create``   -> POST   /memory          (Memory)
  - ``update``/``edit``       -> PATCH  /memory/{id}  (editMemory -> Memory)
  - ``delete``/``invalidate`` -> DELETE /memory/{id}  (soft-delete -> Memory)
  - ``opt_out``  -> POST   /memory/session/{id}/opt-out  (§2.12-only -> D3.5 spec)
"""

from __future__ import annotations

from typing import Any, Optional

from .._exceptions import CortrixError, NotFoundError
from ..types import Memory
from ..types.lists import MemoryList
from ._base import AsyncResource, SyncResource

PATH_MEMORY = "/memory"
PATH_MEMORY_SEARCH = "/memory/search"
# log() targets the LIVE interaction-write route (src/memory/memory_routes.cpp);
# the designed one-shot /memory/log (interaction + MEM02 trigger) is not mounted.
PATH_MEMORY_LOG = "/memory/sessions/{session_id}/interactions"
PATH_MEMORY_SESSIONS = "/memory/sessions"
PATH_MEMORY_EXTRACT = "/memory/extract"  # designed (MEM02); not mounted yet -> D3.5
PATH_MEMORY_ID = "/memory/{id}"
PATH_MEMORY_OPT_OUT = "/memory/session/{session_id}/opt-out"  # §2.12-only -> D3.5 spec


def _search_body(namespace: str, query: str, user_id: str, top_k: int) -> dict[str, Any]:
    return {"namespace": namespace, "query": query, "user_id": user_id, "top_k": top_k}


def _interaction_body(
    namespace: str,
    query: str,
    response: str,
    user_id: str,
    context: Optional[dict[str, Any]],
) -> dict[str, Any]:
    """Body for the live ``POST /memory/sessions/{id}/interactions`` route."""
    body: dict[str, Any] = {
        "namespace": namespace,
        "query_text": query,
        "response_text": response,
        "user_id": user_id,
    }
    if context is not None:
        body["metadata"] = context
    return body


def _log_body(
    namespace: str,
    query: str,
    response: str,
    user_id: str,
    session_id: Optional[str],
    context: Optional[dict[str, Any]],
) -> dict[str, Any]:
    body: dict[str, Any] = {
        "namespace": namespace,
        "query": query,
        "response": response,
        "user_id": user_id,
    }
    if session_id is not None:
        body["session_id"] = session_id
    if context is not None:
        body["context"] = context
    return body


def _list_params(
    namespace: str,
    memory_type: Optional[str],
    include_invalidated: bool,
    limit: int,
    offset: int,
) -> dict[str, Any]:
    params: dict[str, Any] = {
        "namespace": namespace,
        "include_invalidated": include_invalidated,
        "limit": limit,
        "offset": offset,
    }
    if memory_type is not None:
        params["memory_type"] = memory_type
    return params


def _create_body(
    namespace: str, content: str, memory_type: str, metadata: Optional[dict[str, Any]]
) -> dict[str, Any]:
    body: dict[str, Any] = {
        "namespace": namespace,
        "content": content,
        "memory_type": memory_type,
    }
    if metadata is not None:
        body["metadata"] = metadata
    return body


def _edit_body(content: Optional[str], metadata: Optional[dict[str, Any]]) -> dict[str, Any]:
    body: dict[str, Any] = {}
    if content is not None:
        body["content"] = content
    if metadata is not None:
        body["metadata"] = metadata
    return body


class Memory_(SyncResource):
    """Memory operations. ``/memory`` domain.

    (Class is exported as ``Memory`` via the resource module; named ``Memory_``
    internally to avoid clashing with the ``Memory`` dataclass.)
    """

    def search(
        self, namespace: str, query: str, *, user_id: str, top_k: int = 5
    ) -> MemoryList:
        """Semantic memory search. ``POST /memory/search`` -> {memories}."""
        return self._client._request(
            "POST",
            PATH_MEMORY_SEARCH,
            json=_search_body(namespace, query, user_id, top_k),
            response_model=MemoryList,
        )

    def log(
        self,
        namespace: str,
        *,
        query: str,
        response: str,
        user_id: str,
        session_id: Optional[str] = None,
        context: Optional[dict[str, Any]] = None,
    ) -> Any:
        """Log an interaction. Live wire: ``POST /memory/sessions/{id}/interactions``
        (writes the F13 interaction row; the designed one-shot /memory/log with the
        MEM02 auto-extract trigger is not mounted yet -> D3.5)."""
        if not session_id:
            raise ValueError(
                "session_id is required: the live wire logs interactions under "
                "/memory/sessions/{session_id}/interactions"
            )
        path = PATH_MEMORY_LOG.format(session_id=session_id)
        body = _interaction_body(namespace, query, response, user_id, context)
        try:
            return self._client._request("POST", path, json=body)
        except NotFoundError:
            # The live route requires the session row to exist — create it
            # (best-effort: a concurrent create races to a UNIQUE error) and retry.
            try:
                self._client._request("POST", PATH_MEMORY_SESSIONS, json={
                    "session_id": session_id, "namespace": namespace, "user_id": user_id,
                })
            except CortrixError:
                pass
            return self._client._request("POST", path, json=body)

    def extract(
        self,
        namespace: str,
        *,
        query: str,
        response: str,
        user_id: str,
        session_id: Optional[str] = None,
        context: Optional[dict[str, Any]] = None,
    ) -> Any:
        """Trigger LLM extraction over an interaction. ``POST /memory/extract``
        (real-arch MEM02; PartialSuccessById)."""
        return self._client._request(
            "POST",
            PATH_MEMORY_EXTRACT,
            json=_log_body(namespace, query, response, user_id, session_id, context),
        )

    def list(
        self,
        *,
        namespace: str,
        memory_type: Optional[str] = None,
        include_invalidated: bool = False,
        limit: int = 20,
        offset: int = 0,
    ) -> MemoryList:
        """List user memory. ``GET /memory``."""
        return self._client._request(
            "GET",
            PATH_MEMORY,
            params=_list_params(namespace, memory_type, include_invalidated, limit, offset),
            response_model=MemoryList,
        )

    def create(
        self,
        namespace: str,
        content: str,
        *,
        memory_type: str,
        metadata: Optional[dict[str, Any]] = None,
    ) -> Memory:
        """Create a memory. ``POST /memory`` -> Memory."""
        return self._client._request(
            "POST",
            PATH_MEMORY,
            json=_create_body(namespace, content, memory_type, metadata),
            response_model=Memory,
        )

    def update(
        self,
        memory_id: str,
        *,
        content: Optional[str] = None,
        metadata: Optional[dict[str, Any]] = None,
    ) -> Memory:
        """Edit a memory (new user_edit + invalidate old). ``PATCH /memory/{id}``."""
        return self._client._request(
            "PATCH",
            PATH_MEMORY_ID.format(id=memory_id),
            json=_edit_body(content, metadata),
            response_model=Memory,
        )

    # design alias: edit == update (MEM03 §7.bis #28)
    edit = update

    def delete(self, memory_id: str) -> Memory:
        """Soft-delete a memory (status=invalidated). ``DELETE /memory/{id}`` -> Memory."""
        return self._client._request(
            "DELETE", PATH_MEMORY_ID.format(id=memory_id), response_model=Memory
        )

    # design alias: invalidate == delete (MEM03 §7.bis #29, soft-delete)
    invalidate = delete

    def opt_out(self, session_id: str) -> Any:
        """Session memory opt-out. ``POST /memory/session/{id}/opt-out``
        (§2.12-only; P04 spec to be added -> D3.5)."""
        return self._client._request(
            "POST", PATH_MEMORY_OPT_OUT.format(session_id=session_id)
        )


class AsyncMemory(AsyncResource):
    """Async Memory (symmetric to :class:`Memory_`)."""

    async def search(
        self, namespace: str, query: str, *, user_id: str, top_k: int = 5
    ) -> MemoryList:
        return await self._client._request(
            "POST",
            PATH_MEMORY_SEARCH,
            json=_search_body(namespace, query, user_id, top_k),
            response_model=MemoryList,
        )

    async def log(
        self,
        namespace: str,
        *,
        query: str,
        response: str,
        user_id: str,
        session_id: Optional[str] = None,
        context: Optional[dict[str, Any]] = None,
    ) -> Any:
        """Log an interaction (live wire — see :meth:`Memory_.log`). Async."""
        if not session_id:
            raise ValueError(
                "session_id is required: the live wire logs interactions under "
                "/memory/sessions/{session_id}/interactions"
            )
        path = PATH_MEMORY_LOG.format(session_id=session_id)
        body = _interaction_body(namespace, query, response, user_id, context)
        try:
            return await self._client._request("POST", path, json=body)
        except NotFoundError:
            try:
                await self._client._request("POST", PATH_MEMORY_SESSIONS, json={
                    "session_id": session_id, "namespace": namespace, "user_id": user_id,
                })
            except CortrixError:
                pass
            return await self._client._request("POST", path, json=body)

    async def extract(
        self,
        namespace: str,
        *,
        query: str,
        response: str,
        user_id: str,
        session_id: Optional[str] = None,
        context: Optional[dict[str, Any]] = None,
    ) -> Any:
        return await self._client._request(
            "POST",
            PATH_MEMORY_EXTRACT,
            json=_log_body(namespace, query, response, user_id, session_id, context),
        )

    async def list(
        self,
        *,
        namespace: str,
        memory_type: Optional[str] = None,
        include_invalidated: bool = False,
        limit: int = 20,
        offset: int = 0,
    ) -> MemoryList:
        return await self._client._request(
            "GET",
            PATH_MEMORY,
            params=_list_params(namespace, memory_type, include_invalidated, limit, offset),
            response_model=MemoryList,
        )

    async def create(
        self,
        namespace: str,
        content: str,
        *,
        memory_type: str,
        metadata: Optional[dict[str, Any]] = None,
    ) -> Memory:
        return await self._client._request(
            "POST",
            PATH_MEMORY,
            json=_create_body(namespace, content, memory_type, metadata),
            response_model=Memory,
        )

    async def update(
        self,
        memory_id: str,
        *,
        content: Optional[str] = None,
        metadata: Optional[dict[str, Any]] = None,
    ) -> Memory:
        return await self._client._request(
            "PATCH",
            PATH_MEMORY_ID.format(id=memory_id),
            json=_edit_body(content, metadata),
            response_model=Memory,
        )

    edit = update

    async def delete(self, memory_id: str) -> Memory:
        return await self._client._request(
            "DELETE", PATH_MEMORY_ID.format(id=memory_id), response_model=Memory
        )

    invalidate = delete

    async def opt_out(self, session_id: str) -> Any:
        return await self._client._request(
            "POST", PATH_MEMORY_OPT_OUT.format(session_id=session_id)
        )
