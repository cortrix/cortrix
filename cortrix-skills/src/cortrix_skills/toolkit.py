"""CortrixToolKit — 29 methods, 1:1 mirror of the MCP 29 main tools.

Naming SoT (feature design issue 1 / V6 D-15): ``kit.cortrix_<action>`` matches
the MCP ``cortrix_<action>`` tool names (number + business semantics, not
forced literal). Each method calls the **Python SDK** (``self._client.xxx``)
when the SDK exposes a matching Resource.verb; otherwise it falls back to a
**pass-through HTTP call** via ``self._client._request(...)`` against the same
wire contract the MCP server tool uses (feature design section 4 note †). The HTTP
fallback runs through the SDK's own request loop, so the GEN-Agent 4 fields on
``CortrixError`` are preserved either way.

GEN-Agent 4-field pass-through (feature design section 6): methods do **not**
catch ``CortrixError`` — it propagates unchanged (it already carries
``retryable`` / ``category`` / ``retry_after_ms`` / ``structured_data``). The
framework adapters (LangChain / Claude / OpenAI) wrap it into each framework's
error model.

Error codes are passed through from Python SDK (feature design section 7): the Skill
SDK adds **no** ``CX_ERR_*`` prefix of its own.

D3.5 deferred (standalone scope): real LLM round-trips, the spec_lint three-way
check run, and notebook execution are out of scope here — methods are exercised
with mocked SDK / HTTP responses. The ``cortrix_skills_*`` metrics are exported
by cortrix-server (deployment), not by this package (feature design section 1.3 /
section 10.bis).
"""

from __future__ import annotations

import dataclasses
from typing import Any, List, Optional

from cortrix import Cortrix

__all__ = ["CortrixToolKit", "TOOL_METHOD_NAMES"]

# The 29 method names, in MCP server tool order. Adapters iterate this list (rather than
# ``dir(kit)``) so ordering is deterministic and the 1:1 count is explicit.
TOOL_METHOD_NAMES: tuple[str, ...] = (
    # MVP 12 (#1-12)
    "cortrix_health",
    "cortrix_query",
    "cortrix_upload",
    "cortrix_list_documents",
    "cortrix_list_namespaces",
    "cortrix_create_namespace",
    "cortrix_memory_search",
    "cortrix_log_interaction",
    "cortrix_list_interactions",
    "cortrix_document_status",
    "cortrix_add_watcher",
    "cortrix_list_watchers",
    # extended 4 (#13-16)
    "cortrix_cross_ns_query",
    "cortrix_async_upload",
    "cortrix_memory_search_filter",
    "cortrix_memory_extract_trigger",
    # new 4 (#17-20)
    "cortrix_memory_extract",
    "cortrix_task_status",
    "cortrix_cancel_task",
    "cortrix_query_explain",
    # Memory extraction reverse +2 (#21-22)
    "cortrix_memory_get_audit",
    "cortrix_memory_revoke_fact",
    # Memory opt-out reverse +1 (#23)
    "cortrix_memory_opt_out",
    # Batch submit reverse +1 (#24)
    "cortrix_batch_submit",
    # Operation log reverse +1 (#25)
    "cortrix_list_operations",
    # Memory transparency reverse +4 (#26-29)
    "cortrix_memory_list",
    "cortrix_memory_create",
    "cortrix_memory_edit",
    "cortrix_memory_invalidate",
)


def _to_dict(obj: Any) -> Any:
    """Normalize a Python SDK return value to a plain JSON-serializable structure.

    Python SDK response types are plain ``@dataclass`` objects (not pydantic), so they
    have no ``.model_dump()``. This converts a dataclass (recursively) to a dict;
    ``dict`` / ``None`` / primitives pass through unchanged. Lists are mapped
    element-wise. (Verified against cortrix SDK ``types/_generated.py``.)
    """
    if obj is None:
        return None
    if dataclasses.is_dataclass(obj) and not isinstance(obj, type):
        return dataclasses.asdict(obj)
    if isinstance(obj, (list, tuple)):
        return [_to_dict(item) for item in obj]
    return obj


class CortrixToolKit:
    """Cortrix tool surface for framework Agents (29 methods).

    Wraps a Python SDK ``cortrix.Cortrix`` client. Construction is lazy — no network
    call is made until the first method runs (feature design section 12.x:
    invalid ``base_url`` / ``api_key`` surface on first call, not on init).

    Usage::

        kit = CortrixToolKit(base_url="https://cortrix.example.com", api_key="sk-...")
        kit.cortrix_query(query="quarterly revenue", top_k=5)

    Pass ``client=`` to inject a pre-built (or mocked) client instead.
    """

    def __init__(
        self,
        base_url: Optional[str] = None,
        api_key: Optional[str] = None,
        *,
        client: Optional[Cortrix] = None,
        default_namespace: str = "default",
        **client_kwargs: Any,
    ) -> None:
        if client is not None:
            self._client = client
        else:
            if base_url is None:
                raise ValueError(
                    "CortrixToolKit requires base_url (or pass client=). "
                    "Example: CortrixToolKit(base_url='https://cortrix.example.com', api_key='sk-...')"
                )
            self._client = Cortrix(base_url=base_url, api_key=api_key, **client_kwargs)
        self._default_namespace = default_namespace

    # --- lifecycle -----------------------------------------------------------

    def close(self) -> None:
        """Close the underlying HTTP client (feature design section 12.x)."""
        close = getattr(self._client, "close", None)
        if callable(close):
            close()

    def __enter__(self) -> "CortrixToolKit":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    # --- internal helpers ----------------------------------------------------

    def _ns(self, namespace: Optional[str]) -> str:
        return namespace or self._default_namespace

    def _http(
        self,
        method: str,
        path: str,
        *,
        json: Optional[dict] = None,
        params: Optional[dict] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Pass-through HTTP call via the SDK request loop (Python SDK gap fallback).

        Used for the methods whose Python SDK Resource.verb does not exist yet, or whose
        signature diverges from the MCP server wire contract (feature design section 4
        note †). Routing through ``self._client._request`` keeps the GEN-Agent
        4-field ``CortrixError`` mapping identical to the SDK path (no
        re-wrapping, no new error prefix).
        """
        return self._client._request(method, path, json=json, params=params, timeout=timeout)

    # =========================================================================
    # MVP 12 methods (#1-12)
    # =========================================================================

    def cortrix_health(self) -> dict:
        """Check that the Cortrix backend is running and reachable (GET /system/health)."""
        return _to_dict(self._client.system.health())

    def cortrix_query(
        self,
        query: str,
        top_k: int = 5,
        namespaces: Optional[List[str]] = None,
        rerank: bool = True,
    ) -> dict:
        """Semantic search (vector + BM25) across one or more namespaces.

        Args:
            query: search query text.
            top_k: number of results to return (default 5).
            namespaces: target namespaces; single / multiple / ["*"] for all
                (hard cap 100). Defaults to the configured namespace when omitted.
            rerank: enable reranking of fused results (default True).

        Partial failures return data + meta.namespaces_failed (degraded path).
        Pass-through GEN-Agent 4-field error response.
        """
        ns = namespaces if namespaces else [self._default_namespace]
        return _to_dict(self._client.search(ns, query, top_k=top_k, rerank=rerank))

    def cortrix_upload(
        self,
        content: str,
        namespace: str = "",
        filename: str = "",
        metadata: Optional[dict] = None,
    ) -> dict:
        """Upload a document for indexing (async; POST /documents -> task_id).

        Args:
            content: document content (or base64; use cortrix_async_upload for large files).
            namespace: target namespace (uses default if empty).
            filename: optional original filename.
            metadata: optional custom metadata.

        Returns a DocumentTask with task_id; poll cortrix_task_status for progress.
        Pass-through GEN-Agent 4-field error response.
        """
        body: dict = {"namespace": self._ns(namespace), "content": content}
        if filename:
            body["filename"] = filename
        if metadata:
            body["metadata"] = metadata
        return _to_dict(self._http("POST", "/documents", json=body, timeout=120.0))

    def cortrix_list_documents(
        self,
        namespace: str = "",
        limit: int = 50,
        offset: int = 0,
    ) -> dict:
        """List indexed documents in a namespace (GET /documents).

        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(
            self._client.documents.list(self._ns(namespace), limit=limit, offset=offset)
        )

    def cortrix_list_namespaces(self, limit: int = 50, offset: int = 0) -> dict:
        """List authorized namespaces (GET /namespaces).

        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(self._client.namespaces.list(limit=limit, offset=offset))

    def cortrix_create_namespace(self, name: str) -> dict:
        """Create a new namespace for data isolation (POST /namespaces).

        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(self._client.namespaces.create(name))

    def cortrix_memory_search(
        self,
        query: str,
        namespace: str = "",
        top_k: int = 5,
    ) -> dict:
        """Semantic search over conversation memory (POST /memory/search).

        user_id ownership is enforced server-side (memory isolation).
        Pass-through GEN-Agent 4-field error response.
        """
        body = {"namespace": self._ns(namespace), "query": query, "top_k": top_k}
        return _to_dict(self._http("POST", "/memory/search", json=body, timeout=10.0))

    def cortrix_log_interaction(
        self,
        session_id: str,
        query_text: str,
        response_text: str,
        namespace: str = "",
    ) -> dict:
        """Save a conversation turn (user query + assistant response) to memory.

        Backed by POST /interactions (feature design section 4.5.quater).
        Pass-through GEN-Agent 4-field error response.
        """
        body = {
            "namespace": self._ns(namespace),
            "session_id": session_id,
            "query_text": query_text,
            "response_text": response_text,
        }
        return _to_dict(self._http("POST", "/interactions", json=body, timeout=10.0))

    def cortrix_list_interactions(
        self,
        namespace: str,
        user_id: str,
        filter: Optional[dict] = None,
        limit: int = 50,
        offset: int = 0,
    ) -> dict:
        """List interactions for a user_id (GET /interactions).

        ``filter`` whitelist (5 fields): session_id / namespace_id / from_ts /
        to_ts / sort_order; out-of-whitelist keys pass through the backend's
        CX_ERR_PGCORTRIX_INVALID_FILTER. Pass-through GEN-Agent 4-field error response.
        """
        params: dict = {"namespace": namespace, "user_id": user_id, "limit": limit, "offset": offset}
        if filter:
            params["filter"] = filter
        return _to_dict(self._http("GET", "/interactions", params=params, timeout=10.0))

    def cortrix_document_status(self, doc_id: str, namespace: str = "") -> dict:
        """Get a document's processing status / details (GET /documents/{id}).

        Args:
            doc_id: document id returned from upload.
            namespace: reserved for forward compatibility (resolved by id).

        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(self._client.documents.status(doc_id))

    def cortrix_add_watcher(
        self,
        data_dir: str,
        namespace: str = "",
        recursive: bool = True,
    ) -> dict:
        """Add a directory watcher to auto-index files (POST /watch).

        Args:
            data_dir: path of the directory to watch.
            namespace: target namespace (uses default if empty).
            recursive: watch subdirectories (default True).

        New/modified files fan out to all namespaces subscribed to the directory.
        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(
            self._client.watchers.add(data_dir, [self._ns(namespace)], recursive=recursive)
        )

    def cortrix_list_watchers(self) -> dict:
        """List all configured directory watchers (GET /watch).

        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(self._client.watchers.list())

    # =========================================================================
    # Extended 4 methods (#13-16)
    # =========================================================================

    def cortrix_cross_ns_query(
        self,
        query: str,
        namespaces: List[str],
        top_k: int = 10,
        rerank: bool = True,
    ) -> dict:
        """Query across multiple namespaces (cross-NS query; POST /query with a namespaces array).

        Args:
            query: search query text.
            namespaces: target namespaces; multiple names or ["*"] for all (hard cap 100).
            top_k: number of results (default 10).
            rerank: enable cross-namespace reranking (default True).

        Partial failures return 200 + meta.namespaces_failed (degraded path).
        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(self._client.search(namespaces, query, top_k=top_k, rerank=rerank))

    def cortrix_async_upload(
        self,
        content: str,
        namespace: str = "",
        filename: str = "",
        metadata: Optional[dict] = None,
    ) -> dict:
        """Asynchronously upload a large document (async task; POST /documents -> task_id).

        Always async: returns a DocumentTask with task_id; poll cortrix_task_status.
        Pass-through GEN-Agent 4-field error response.
        """
        body: dict = {"namespace": self._ns(namespace), "content": content}
        if filename:
            body["filename"] = filename
        if metadata:
            body["metadata"] = metadata
        return _to_dict(self._http("POST", "/documents", json=body, timeout=120.0))

    def cortrix_memory_search_filter(
        self,
        query: str,
        memory_type: str,
        namespace: str = "",
        top_k: int = 5,
    ) -> dict:
        """Memory search filtered by memory type (memory decay/memory isolation; POST /memory/search).

        Args:
            query: search query text.
            memory_type: one of fact / preference / event.
            namespace: target namespace (uses default if empty).
            top_k: number of results (default 5).

        Pass-through GEN-Agent 4-field error response.
        """
        body = {
            "namespace": self._ns(namespace),
            "query": query,
            "top_k": top_k,
            "memory_type": memory_type,
        }
        return _to_dict(self._http("POST", "/memory/search", json=body, timeout=10.0))

    def cortrix_memory_extract_trigger(
        self,
        namespace: str = "",
        session_id: str = "",
    ) -> dict:
        """Manually trigger memory extraction for a session (POST /memory/extract).

        Args:
            namespace: target namespace (uses default if empty).
            session_id: session to extract memories from.

        When category=quota (LLM budget exceeded) the error is retryable=false.
        Pass-through GEN-Agent 4-field error response.
        """
        body: dict = {"namespace": self._ns(namespace)}
        if session_id:
            body["session_id"] = session_id
        return _to_dict(self._http("POST", "/memory/extract", json=body, timeout=60.0))

    # =========================================================================
    # New 4 methods (#17-20)
    # =========================================================================

    def cortrix_memory_extract(
        self,
        messages: List[dict],
        namespace: str = "",
    ) -> dict:
        """Extract memories from a conversation in one shot (memory extraction; POST /memory/extract).

        Args:
            messages: conversation turns, each {role, content}.
            namespace: target namespace (uses default if empty).

        Batch extraction uses PartialSuccessById semantics; when category=quota the
        error is retryable=false. Pass-through GEN-Agent 4-field error response.
        """
        body = {"namespace": self._ns(namespace), "messages": messages}
        return _to_dict(self._http("POST", "/memory/extract", json=body, timeout=60.0))

    def cortrix_task_status(self, task_id: str) -> dict:
        """Query async upload task progress (async task; GET /documents/tasks/{task_id}/progress).

        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(self._client.documents.task_progress(task_id))

    def cortrix_cancel_task(self, task_id: str) -> dict:
        """Cancel an async upload task (async task; DELETE /documents/tasks/{task_id}).

        Returns 409 (passed through) if the task already finished and cannot be cancelled.
        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(self._client.documents.cancel_task(task_id))

    def cortrix_query_explain(
        self,
        query: str,
        top_k: int = 5,
        namespaces: Optional[List[str]] = None,
        rerank: bool = True,
    ) -> dict:
        """Semantic search with explain enabled (POST /query?explain=true).

        Surfaces Class-A data-integrity fields (coverage_ratio / namespaces_failed)
        and the Class-B LLM-path explain section (ARCH section 1.6) into the
        response, so an agent can reason about retrieval quality.
        Pass-through GEN-Agent 4-field error response.
        """
        ns = namespaces if namespaces else [self._default_namespace]
        body = {"query": query, "namespaces": ns, "top_k": top_k, "rerank": rerank}
        return _to_dict(
            self._http("POST", "/query", json=body, params={"explain": "true"})
        )

    # =========================================================================
    # Memory extraction reverse +2 (#21-22)
    # =========================================================================

    def cortrix_memory_get_audit(
        self,
        memory_id: Optional[str] = None,
        namespace: str = "",
        limit: int = 50,
    ) -> dict:
        """Query the memory audit log (memory extraction; reuses the operation log).

        Args:
            memory_id: filter to a single memory's audit trail (optional).
            namespace: target namespace (uses default if empty).
            limit: max entries to return (default 50).

        Pass-through GEN-Agent 4-field error response.
        """
        params: dict = {"namespace": self._ns(namespace), "limit": limit}
        if memory_id:
            params["memory_id"] = memory_id
        return _to_dict(self._http("GET", "/memory/audit", params=params, timeout=10.0))

    def cortrix_memory_revoke_fact(
        self,
        memory_id: str,
        namespace: str = "",
    ) -> dict:
        """Self-service revoke of an auto-extracted fact (memory extraction D6).

        Marks the fact auto_revoke_eligible; the physical row is retained
        (full-retention model). Pass-through GEN-Agent 4-field error response.
        """
        body = {"namespace": self._ns(namespace)}
        return _to_dict(
            self._http("POST", f"/memory/{memory_id}/revoke", json=body, timeout=10.0)
        )

    # =========================================================================
    # Memory opt-out reverse +1 (#23)
    # =========================================================================

    def cortrix_memory_opt_out(
        self,
        session_id: str,
        opt_out: bool = True,
        namespace: str = "",
    ) -> dict:
        """Per-session memory opt-out (and opt-out revoke), memory opt-out D2 + D5 combined.

        Args:
            session_id: session to opt out of memory extraction.
            opt_out: True to opt out, False to revoke the opt-out.
            namespace: target namespace (uses default if empty).

        Pass-through GEN-Agent 4-field error response.
        """
        body = {"namespace": self._ns(namespace), "session_id": session_id, "opt_out": opt_out}
        return _to_dict(self._http("POST", "/memory/opt-out", json=body, timeout=10.0))

    # =========================================================================
    # Batch submit reverse +1 (#24)
    # =========================================================================

    def cortrix_batch_submit(
        self,
        namespace: str,
        documents: List[dict],
        async_: bool = True,
        on_duplicate: str = "skip",
    ) -> dict:
        """Submit up to 100 documents to a namespace in one batch.

        Args:
            namespace: target namespace.
            documents: up to 100 items, each {doc_id, content, metadata?}.
            async_: must be True in V1.
            on_duplicate: skip / overwrite / error.

        Returns a results list (same order as documents) + structured_data
        (succeeded / failed / total / coverage_ratio). A batch-level failure
        returns the GEN-Agent error response.
        """
        body = {
            "namespace": namespace,
            "documents": documents,
            "async": async_,
            "on_duplicate": on_duplicate,
        }
        return _to_dict(self._http("POST", "/documents/batch", json=body, timeout=120.0))

    # =========================================================================
    # Operation log reverse +1 (#25)
    # =========================================================================

    def cortrix_list_operations(
        self,
        user_id: Optional[str] = None,
        namespace: Optional[str] = None,
        action: Optional[str] = None,
        start_time: Optional[str] = None,
        end_time: Optional[str] = None,
        limit: int = 50,
        cursor: Optional[str] = None,
    ) -> dict:
        """Query the operation log (operation log; CE 30-day retention + 100K cap).

        Args:
            user_id: defaults to the current agent user_id when omitted.
            namespace: filter by namespace.
            action: one of the operation log action enum values.
            start_time / end_time: ISO 8601 time range.
            limit: max rows (default 50, max 200).
            cursor: opaque pagination token.

        Returns operations + structured_data (total / has_more / next_cursor).
        Pass-through GEN-Agent 4-field error response.
        """
        params: dict = {"limit": min(limit, 200)}
        for key, value in (
            ("user_id", user_id),
            ("namespace", namespace),
            ("action", action),
            ("start_time", start_time),
            ("end_time", end_time),
            ("cursor", cursor),
        ):
            if value is not None:
                params[key] = value
        return _to_dict(self._http("GET", "/operations", params=params, timeout=10.0))

    # =========================================================================
    # Memory transparency reverse +4 (#26-29) — Memory Transparency CRUD
    # =========================================================================

    def cortrix_memory_list(
        self,
        namespace: Optional[str] = None,
        memory_type: Optional[str] = None,
        include_invalidated: bool = False,
        limit: int = 50,
        offset: int = 0,
    ) -> dict:
        """List the current user_id's memories (memory transparency endpoint 1; GET /memory).

        Args:
            namespace: defaults to the current user_id's namespace when omitted.
            memory_type: filter by type (fact / preference / event).
            include_invalidated: include soft-deleted (status=invalidated) memories
                for transparency queries.
            limit: max rows (default 50, max 200).
            offset: pagination offset.

        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(
            self._client.memory.list(
                namespace=self._ns(namespace),
                memory_type=memory_type,
                include_invalidated=include_invalidated,
                limit=min(limit, 200),
                offset=offset,
            )
        )

    def cortrix_memory_create(
        self,
        namespace: str,
        content: str,
        memory_type: str,
        metadata: Optional[dict] = None,
    ) -> dict:
        """Create a memory (memory transparency endpoint 2; POST /memory).

        The POST path does not pass through LLM quality filtering (memory transparency issue 4);
        extraction_method is set to 'user_create' server-side.
        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(
            self._client.memory.create(
                namespace, content, memory_type=memory_type, metadata=metadata
            )
        )

    def cortrix_memory_edit(
        self,
        memory_id: str,
        content: Optional[str] = None,
        metadata: Optional[dict] = None,
    ) -> dict:
        """Edit an existing memory's content / metadata (memory transparency endpoint 3; PATCH /memory/{id}).

        metadata merge semantics (not replace); extraction_method is overwritten
        to 'user_edit' server-side. Semantically a new user_edit memory +
        invalidation of the old one, hence PATCH not PUT.
        Pass-through GEN-Agent 4-field error response.
        """
        return _to_dict(
            self._client.memory.edit(memory_id, content=content, metadata=metadata)
        )

    def cortrix_memory_invalidate(
        self,
        memory_id: str,
        namespace: Optional[str] = None,
        reason: Optional[str] = None,
    ) -> dict:
        """Soft-delete a memory (memory transparency endpoint 4; DELETE /memory/{id}).

        Sets status=invalidated + revoked_at + deleted_by_user_id; the physical row
        is retained (never hard-deleted) per the memory extraction D9 full-retention model and
        the memory transparency vision. ``reason`` is written to
        metadata_json.invalidation_reason.

        Errors pass through: CX_ERR_MEMORY_NOT_FOUND / CX_ERR_MEMORY_USER_MISMATCH /
        CX_ERR_MEMORY_ALREADY_INVALIDATED / CX_ERR_MEMORY_INVALIDATE_FAILED / CX_ERR_MEMORY_QUOTA.
        Pass-through GEN-Agent 4-field error response.
        """
        # The Python SDK invalidate(==delete) takes only memory_id; namespace / reason
        # are MCP-wire query params, so when supplied we use the HTTP fallback to
        # preserve the full MCP server contract (no Python SDK change).
        if namespace is None and reason is None:
            return _to_dict(self._client.memory.invalidate(memory_id))
        params: dict = {}
        if namespace:
            params["namespace"] = namespace
        if reason:
            params["reason"] = reason
        return _to_dict(self._http("DELETE", f"/memory/{memory_id}", params=params, timeout=10.0))
