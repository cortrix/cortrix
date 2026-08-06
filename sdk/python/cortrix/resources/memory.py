"""Memory resource. ``/memory`` domain (ARCH + the memory features).

This resource follows the implemented HTTP architecture. Method -> endpoint:
  - ``search``   -> POST   /memory/search   ({results,total_results,...})
  - ``log``      -> POST   /memory/sessions/{id}/interactions  (live wire)
  - ``extract``  -> POST   /memory/extract  (real-arch, memory extraction; PartialSuccessById)
  - ``list``     -> GET    /memory          ({memories,total,...})
  - ``create``   -> POST   /memory          (MemoryCreateAck)
  - ``update``/``edit``       -> PATCH  /memory/{id}  (MemoryEditAck)
  - ``delete``/``invalidate`` -> DELETE /memory/{id}  (MemoryDeleteAck)
  - ``opt_out``  -> POST   /memory/session/{id}/opt-out  (spec to follow)
"""

from __future__ import annotations

from typing import Any, Optional

from .._exceptions import CortrixError, NotFoundError
from ..types import MemoryCreateAck, MemoryDeleteAck, MemoryEditAck, MemorySearchResponse
from ..types.lists import MemoryList
from ._base import AsyncResource, SyncResource

PATH_MEMORY = "/memory"
PATH_MEMORY_SEARCH = "/memory/search"
# log() targets the LIVE interaction-write route (src/memory/memory_routes.cpp);
# the designed one-shot /memory/log (interaction + memory extraction trigger) is not mounted.
PATH_MEMORY_LOG = "/memory/sessions/{session_id}/interactions"
PATH_MEMORY_SESSIONS = "/memory/sessions"
PATH_MEMORY_EXTRACT = "/memory/extract"
PATH_MEMORY_ID = "/memory/{id}"
PATH_MEMORY_OPT_OUT = "/memory/session/{session_id}/opt-out"  # spec to follow


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
        "query_text": query,
        "response_text": response,
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


def _edit_body(
    content: Optional[str],
    metadata: Optional[dict[str, Any]],
    namespace: Optional[str],
) -> dict[str, Any]:
    body: dict[str, Any] = {}
    if namespace is not None:
        body["namespace"] = namespace
    if content is not None:
        body["content"] = content
    if metadata is not None:
        body["metadata"] = metadata
    return body


def _delete_params(namespace: Optional[str]) -> Optional[dict[str, Any]]:
    if namespace is None:
        return None
    return {"namespace": namespace}


class Memory_(SyncResource):
    """Memory operations. ``/memory`` domain.

    (Class is exported as ``Memory`` via the resource module; named ``Memory_``
    internally to avoid clashing with the ``Memory`` dataclass.)
    """

    def __init__(self, client: Any) -> None:
        super().__init__(client)
        self._memory_namespaces: dict[str, str] = {}
        self._session_namespaces: dict[str, str] = {}

    def _remember_memory(self, memory_id: Optional[str], namespace: Optional[str]) -> None:
        if memory_id and namespace:
            self._memory_namespaces[memory_id] = namespace

    def _remember_session(self, session_id: Optional[str], namespace: Optional[str]) -> None:
        if session_id and namespace:
            self._session_namespaces[session_id] = namespace

    def search(
        self, namespace: str, query: str, *, user_id: str, top_k: int = 5
    ) -> MemorySearchResponse:
        """Semantic memory search. ``POST /memory/search`` -> {results}."""
        return self._client._request(
            "POST",
            PATH_MEMORY_SEARCH,
            json=_search_body(namespace, query, user_id, top_k),
            response_model=MemorySearchResponse,
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
        (writes the agent trace interaction row; the designed one-shot /memory/log with the
        Memory extraction auto-extract trigger is not mounted yet)."""
        if not session_id:
            raise ValueError(
                "session_id is required: the live wire logs interactions under "
                "/memory/sessions/{session_id}/interactions"
            )
        path = PATH_MEMORY_LOG.format(session_id=session_id)
        body = _interaction_body(namespace, query, response, user_id, context)
        try:
            result = self._client._request("POST", path, json=body)
            self._remember_session(session_id, namespace)
            return result
        except NotFoundError:
            # The live route requires the session row to exist — create it
            # (best-effort: a concurrent create races to a UNIQUE error) and retry.
            try:
                self._client._request("POST", PATH_MEMORY_SESSIONS, json={
                    "session_id": session_id, "namespace": namespace, "user_id": user_id,
                })
            except CortrixError:
                pass
            result = self._client._request("POST", path, json=body)
            self._remember_session(session_id, namespace)
            return result

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
        (real-arch memory extraction; PartialSuccessById)."""
        result = self._client._request(
            "POST",
            PATH_MEMORY_EXTRACT,
            json=_log_body(namespace, query, response, user_id, session_id, context),
        )
        self._remember_session(session_id, namespace)
        return result

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
    ) -> MemoryCreateAck:
        """Create a memory. ``POST /memory`` -> acknowledgment."""
        result = self._client._request(
            "POST",
            PATH_MEMORY,
            json=_create_body(namespace, content, memory_type, metadata),
            response_model=MemoryCreateAck,
        )
        self._remember_memory(result.memory_id, namespace)
        return result

    def update(
        self,
        memory_id: str,
        *,
        content: Optional[str] = None,
        metadata: Optional[dict[str, Any]] = None,
        namespace: Optional[str] = None,
    ) -> MemoryEditAck:
        """Edit a memory (new user_edit + invalidate old). ``PATCH /memory/{id}``."""
        effective_namespace = namespace or self._memory_namespaces.get(memory_id)
        result = self._client._request(
            "PATCH",
            PATH_MEMORY_ID.format(id=memory_id),
            json=_edit_body(content, metadata, effective_namespace),
            response_model=MemoryEditAck,
        )
        self._remember_memory(result.invalidated_memory_id or memory_id, effective_namespace)
        self._remember_memory(result.new_memory_id or result.memory_id, effective_namespace)
        return result

    # design alias: edit == update (memory transparency #28)
    edit = update

    def delete(self, memory_id: str, *, namespace: Optional[str] = None) -> MemoryDeleteAck:
        """Soft-delete a memory. ``DELETE /memory/{id}`` -> acknowledgment."""
        effective_namespace = namespace or self._memory_namespaces.get(memory_id)
        return self._client._request(
            "DELETE",
            PATH_MEMORY_ID.format(id=memory_id),
            params=_delete_params(effective_namespace),
            response_model=MemoryDeleteAck,
        )

    # design alias: invalidate == delete (memory transparency #29, soft-delete)
    invalidate = delete

    def opt_out(self, session_id: str, *, namespace: Optional[str] = None) -> Any:
        """Session memory opt-out. ``POST /memory/session/{id}/opt-out``
        (API spec to be added)."""
        effective_namespace = namespace or self._session_namespaces.get(session_id)
        return self._client._request(
            "POST",
            PATH_MEMORY_OPT_OUT.format(session_id=session_id),
            json={"namespace": effective_namespace} if effective_namespace else None,
        )


class AsyncMemory(AsyncResource):
    """Async Memory (symmetric to :class:`Memory_`)."""

    def __init__(self, client: Any) -> None:
        super().__init__(client)
        self._memory_namespaces: dict[str, str] = {}
        self._session_namespaces: dict[str, str] = {}

    def _remember_memory(self, memory_id: Optional[str], namespace: Optional[str]) -> None:
        if memory_id and namespace:
            self._memory_namespaces[memory_id] = namespace

    def _remember_session(self, session_id: Optional[str], namespace: Optional[str]) -> None:
        if session_id and namespace:
            self._session_namespaces[session_id] = namespace

    async def search(
        self, namespace: str, query: str, *, user_id: str, top_k: int = 5
    ) -> MemorySearchResponse:
        return await self._client._request(
            "POST",
            PATH_MEMORY_SEARCH,
            json=_search_body(namespace, query, user_id, top_k),
            response_model=MemorySearchResponse,
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
            result = await self._client._request("POST", path, json=body)
            self._remember_session(session_id, namespace)
            return result
        except NotFoundError:
            try:
                await self._client._request("POST", PATH_MEMORY_SESSIONS, json={
                    "session_id": session_id, "namespace": namespace, "user_id": user_id,
                })
            except CortrixError:
                pass
            result = await self._client._request("POST", path, json=body)
            self._remember_session(session_id, namespace)
            return result

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
        result = await self._client._request(
            "POST",
            PATH_MEMORY_EXTRACT,
            json=_log_body(namespace, query, response, user_id, session_id, context),
        )
        self._remember_session(session_id, namespace)
        return result

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
    ) -> MemoryCreateAck:
        result = await self._client._request(
            "POST",
            PATH_MEMORY,
            json=_create_body(namespace, content, memory_type, metadata),
            response_model=MemoryCreateAck,
        )
        self._remember_memory(result.memory_id, namespace)
        return result

    async def update(
        self,
        memory_id: str,
        *,
        content: Optional[str] = None,
        metadata: Optional[dict[str, Any]] = None,
        namespace: Optional[str] = None,
    ) -> MemoryEditAck:
        effective_namespace = namespace or self._memory_namespaces.get(memory_id)
        result = await self._client._request(
            "PATCH",
            PATH_MEMORY_ID.format(id=memory_id),
            json=_edit_body(content, metadata, effective_namespace),
            response_model=MemoryEditAck,
        )
        self._remember_memory(result.invalidated_memory_id or memory_id, effective_namespace)
        self._remember_memory(result.new_memory_id or result.memory_id, effective_namespace)
        return result

    edit = update

    async def delete(
        self, memory_id: str, *, namespace: Optional[str] = None
    ) -> MemoryDeleteAck:
        effective_namespace = namespace or self._memory_namespaces.get(memory_id)
        return await self._client._request(
            "DELETE",
            PATH_MEMORY_ID.format(id=memory_id),
            params=_delete_params(effective_namespace),
            response_model=MemoryDeleteAck,
        )

    invalidate = delete

    async def opt_out(self, session_id: str, *, namespace: Optional[str] = None) -> Any:
        effective_namespace = namespace or self._session_namespaces.get(session_id)
        return await self._client._request(
            "POST",
            PATH_MEMORY_OPT_OUT.format(session_id=session_id),
            json={"namespace": effective_namespace} if effective_namespace else None,
        )
