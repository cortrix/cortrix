"""MVP 12 tools — migrated from mcp-server/cortrix_mcp_server.py and adapted to the
GEN-Agent two-layer 4-field schema, calling the frozen API spec endpoints.

Endpoint reconciliation vs the MVP single-file server (verified against the live backend
route table in src/server, not just api/paths/*.yaml):
  * health           -> GET  /system/health/live    (deploy/health_routes.cpp; the backend
                        exposes /system/health/{live,ready} + a bare /health, but no bare
                        /system/health — the prior "/system/health" call 404'd)
  * query            -> POST /query  body {query, namespaces:[...], top_k, rerank}
                        (MVP used single `namespace` + search_config — API spec uses a namespaces array)
  * upload           -> POST /documents body {namespace, content, filename} async -> task_id
                        (JSON content-upload entry. The multipart POST /namespaces/{ns}/documents
                        is a *second, equally live* entry — file uploads — not stale; both routes
                        are served, double-track by design.)
  * list_documents   -> GET  /documents?namespace&limit&offset
  * list_namespaces  -> GET  /namespaces
  * create_namespace -> POST /namespaces
  * memory_search    -> POST /memory/search
  * document_status  -> GET  /documents/{id}?namespace=...
                        (API spec getDocument; live storage is namespace-scoped)
  * add_watcher      -> POST /watch  body {path, target_namespaces:[...]}
                        (MVP used /connector/watchers {data_dir, namespace_name} — stale)
  * list_watchers    -> GET  /watch
  * log_interaction  -> POST /memory/sessions/{session_id}/interactions
                        (session_id in the path; MVP-era POST /interactions did not exist)
  * list_interactions-> GET  /interactions          (live; renamed from MVP cortrix_list_sessions
                        per feature section 4.5.quater 5-layer alignment; namespace -> namespace_id)
"""

from __future__ import annotations

import os
from typing import Optional

from ..transport import CORTRIX_NAMESPACE, request


def register(mcp) -> None:
    @mcp.tool()
    def cortrix_health() -> dict:
        """Check that the Cortrix backend is running and reachable (GET /system/health/live)."""
        return request("GET", "/system/health/live", timeout=5.0)

    @mcp.tool()
    def cortrix_query(
        query: str,
        top_k: int = 5,
        namespaces: Optional[list[str]] = None,
        rerank: bool = True,
    ) -> dict:
        """Semantic search (vector + BM25) across one or more namespaces.

        Args:
            query: search query text.
            top_k: number of results to return (default 5).
            namespaces: target namespaces; single / multiple / ["*"] for all (hard cap 100).
                Defaults to the configured CORTRIX_NAMESPACE when omitted.
            rerank: enable reranking of fused results (default True).

        Partial failures return data + structured_data.namespaces_failed (degraded path).
        """
        ns = namespaces if namespaces else [CORTRIX_NAMESPACE]
        body = {"query": query, "namespaces": ns, "top_k": top_k, "rerank": rerank}
        return request("POST", "/query", json_body=body)

    @mcp.tool()
    def cortrix_upload(
        content: str,
        namespace: str = "",
        filename: str = "",
        metadata: Optional[dict] = None,
    ) -> dict:
        """Upload a document for indexing (async; POST /documents -> task_id).

        Args:
            content: document content (or base64; use async_upload for large files).
            namespace: target namespace (uses default if empty).
            filename: optional original filename.
            metadata: optional custom metadata.

        Returns a DocumentTask with task_id; poll cortrix_task_status for progress.
        """
        ns = namespace or CORTRIX_NAMESPACE
        body: dict = {"namespace": ns, "content": content}
        if filename:
            body["filename"] = filename
        if metadata:
            body["metadata"] = metadata
        return request("POST", "/documents", json_body=body, timeout=120.0)

    @mcp.tool()
    def cortrix_list_documents(
        namespace: str = "",
        limit: int = 50,
        offset: int = 0,
    ) -> dict:
        """List indexed documents in a namespace (GET /documents)."""
        ns = namespace or CORTRIX_NAMESPACE
        params = {"namespace": ns, "limit": limit, "offset": offset}
        return request("GET", "/documents", params=params, timeout=10.0)

    @mcp.tool()
    def cortrix_list_namespaces(limit: int = 50, offset: int = 0) -> dict:
        """List authorized namespaces (GET /namespaces)."""
        return request(
            "GET", "/namespaces", params={"limit": limit, "offset": offset}, timeout=10.0
        )

    @mcp.tool()
    def cortrix_create_namespace(name: str) -> dict:
        """Create a new namespace for data isolation (POST /namespaces)."""
        return request("POST", "/namespaces", json_body={"name": name}, timeout=10.0)

    @mcp.tool()
    def cortrix_memory_search(
        query: str,
        namespace: str = "",
        top_k: int = 5,
    ) -> dict:
        """Semantic search over conversation memory (POST /memory/search).

        user_id ownership is enforced server-side (memory isolation).
        """
        ns = namespace or CORTRIX_NAMESPACE
        body = {"namespace": ns, "query": query, "top_k": top_k}
        return request("POST", "/memory/search", json_body=body, timeout=10.0)

    @mcp.tool()
    def cortrix_log_interaction(
        session_id: str,
        query_text: str,
        response_text: str,
        namespace: str = "",
    ) -> dict:
        """Save a conversation turn (user query + assistant response) to memory.

        Backed by POST /memory/sessions/{session_id}/interactions (memory_routes.cpp): the
        session id is carried in the URL path; the body holds namespace + query_text +
        response_text (all required server-side). Append is gated by memory isolation session ownership.
        """
        ns = namespace or CORTRIX_NAMESPACE
        body = {
            "namespace": ns,
            "query_text": query_text,
            "response_text": response_text,
        }
        path = f"/memory/sessions/{session_id}/interactions"
        try:
            return request("POST", path, json_body=body, timeout=10.0)
        except Exception as exc:  # noqa: BLE001 - retry only for the live session precondition.
            if "Session not found" not in str(exc) and "CX_ERR_MEM04_SESSION_NOT_FOUND" not in str(exc):
                raise
            request(
                "POST",
                "/memory/sessions",
                json_body={"session_id": session_id, "namespace": ns, "user_id": "default"},
                timeout=10.0,
            )
            return request("POST", path, json_body=body, timeout=10.0)

    @mcp.tool()
    def cortrix_list_interactions(
        namespace: str,
        user_id: str,
        filter: Optional[dict] = None,
        limit: int = 50,
        offset: int = 0,
    ) -> dict:
        """List interactions for a user_id (GET /interactions).

        Backed by the live GET /interactions route (observability_routes.cpp); per-NS
        storage, so the namespace selects the store (sent as namespace_id).

        ``filter`` is a convenience sub-object whose whitelisted keys (session_id /
        namespace_id / from_ts / to_ts / sort_order) are flattened onto the HTTP query
        string, since the backend reads them as flat params (from_ts -> from_timestamp,
        to_ts -> to_timestamp). Out-of-whitelist keys are dropped here rather than sent.
        """
        params: dict = {
            "namespace_id": namespace,
            "user_id": user_id,
            "limit": limit,
            "offset": offset,
        }
        if filter:
            key_map = {
                "session_id": "session_id",
                "namespace_id": "namespace_id",
                "from_ts": "from_timestamp",
                "to_ts": "to_timestamp",
                "sort_order": "sort_order",
            }
            for k, backend_key in key_map.items():
                if filter.get(k) is not None:
                    params[backend_key] = filter[k]
        return request("GET", "/interactions", params=params, data_key="interactions", timeout=10.0)

    @mcp.tool()
    def cortrix_document_status(doc_id: str, namespace: str = "") -> dict:
        """Get a document's processing status / details (GET /documents/{id}?namespace=...).

        Args:
            doc_id: document id returned from upload.
            namespace: target namespace (uses default if empty).
        """
        ns = namespace or CORTRIX_NAMESPACE
        return request("GET", f"/documents/{doc_id}", params={"namespace": ns}, timeout=10.0)

    @mcp.tool()
    def cortrix_add_watcher(
        data_dir: str,
        namespace: str = "",
        recursive: bool = True,
    ) -> dict:
        """Add a directory watcher to auto-index files (POST /watch).

        Args:
            data_dir: absolute path of the directory to watch.
            namespace: target namespace (uses default if empty).
            recursive: watch subdirectories (default True).

        New/modified files fan out to all namespaces subscribed to the directory.
        """
        ns = namespace or CORTRIX_NAMESPACE
        if not os.path.isdir(data_dir):
            # Local pre-check mirrors MVP; reported as a schema-validation failure.
            from ..errors import tool_error

            raise tool_error(
                "CX_ERR_MCP_SCHEMA_VALIDATION_FAIL",
                message=f"Directory not found: {data_dir}",
                structured_data={"field": "data_dir", "value": data_dir},
            )
        body = {"path": data_dir, "target_namespaces": [ns], "recursive": recursive}
        return request("POST", "/watch", json_body=body, timeout=10.0)

    @mcp.tool()
    def cortrix_list_watchers() -> dict:
        """List all configured directory watchers (GET /watch)."""
        return request("GET", "/watch", data_key="watchers", timeout=10.0)
