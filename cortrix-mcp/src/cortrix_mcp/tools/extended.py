"""Extended 4 tools (feature design section 4.2): #13-#16.

  #13 cortrix_cross_ns_query        -> POST /query  (namespaces array, cross-NS query)
  #14 cortrix_async_upload          -> POST /documents (async task, async only)
  #15 cortrix_memory_search_filter  -> POST /memory/search (memory_type filter, memory decay/memory isolation)
  #16 cortrix_memory_extract_trigger-> POST /memory/extract (manual memory extraction trigger)
"""

from __future__ import annotations

from typing import Optional

from ..transport import CORTRIX_NAMESPACE, request


def register(mcp) -> None:
    @mcp.tool()
    def cortrix_cross_ns_query(
        query: str,
        namespaces: list[str],
        top_k: int = 10,
        rerank: bool = True,
    ) -> dict:
        """Query across multiple namespaces (cross-NS query; POST /query with a namespaces array).

        Args:
            query: search query text.
            namespaces: target namespaces; multiple names or ["*"] for all (hard cap 100).
            top_k: number of results (default 10).
            rerank: enable cross-namespace reranking (default True).

        Partial failures return 200 + structured_data.namespaces_failed (degraded path).
        """
        body = {"query": query, "namespaces": namespaces, "top_k": top_k, "rerank": rerank}
        return request("POST", "/query", json_body=body)

    @mcp.tool()
    def cortrix_async_upload(
        content: str,
        namespace: str = "",
        filename: str = "",
        metadata: Optional[dict] = None,
    ) -> dict:
        """Asynchronously upload a large document (async task; POST /documents -> task_id).

        Always async: returns a DocumentTask with task_id; poll cortrix_task_status.
        """
        ns = namespace or CORTRIX_NAMESPACE
        body: dict = {"namespace": ns, "content": content}
        if filename:
            body["filename"] = filename
        if metadata:
            body["metadata"] = metadata
        return request("POST", "/documents", json_body=body, timeout=120.0)

    @mcp.tool()
    def cortrix_memory_search_filter(
        query: str,
        memory_type: str,
        namespace: str = "",
        top_k: int = 5,
    ) -> dict:
        """Memory search filtered by memory type (memory decay/memory isolation; POST /memory/search).

        Args:
            query: search query text.
            memory_type: one of fact / preference / event (API spec memory enum).
            namespace: target namespace (uses default if empty).
            top_k: number of results (default 5).
        """
        ns = namespace or CORTRIX_NAMESPACE
        body = {"namespace": ns, "query": query, "top_k": top_k, "memory_type": memory_type}
        return request("POST", "/memory/search", json_body=body, timeout=10.0)

    @mcp.tool()
    def cortrix_memory_extract_trigger(
        namespace: str = "",
        session_id: str = "",
    ) -> dict:
        """Manually trigger memory extraction for a session (POST /memory/extract).

        Args:
            namespace: target namespace (uses default if empty).
            session_id: session to extract memories from.

        When category=quota (LLM budget exceeded) the error is retryable=false.
        """
        ns = namespace or CORTRIX_NAMESPACE
        body: dict = {"namespace": ns}
        if session_id:
            body["session_id"] = session_id
        return request("POST", "/memory/extract", json_body=body, timeout=60.0)
