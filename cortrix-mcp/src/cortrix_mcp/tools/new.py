"""New 4 tools (feature design section 4.3): #17-#20.

  #17 cortrix_memory_extract  -> POST /memory/extract           (one-shot extraction from text, MEM02 D11)
  #18 cortrix_task_status     -> GET  /documents/tasks/{id}/progress (F42)
  #19 cortrix_cancel_task     -> DELETE /documents/tasks/{id}    (F42 D3)
  #20 cortrix_query_explain   -> POST /query?explain=true        (Class-A/B fields, ARCH section 1.6)
"""

from __future__ import annotations

from typing import Optional

from ..transport import CORTRIX_NAMESPACE, request


def register(mcp) -> None:
    @mcp.tool()
    def cortrix_memory_extract(
        messages: list[dict],
        namespace: str = "",
    ) -> dict:
        """Extract memories from a conversation in one shot (MEM02; POST /memory/extract).

        Args:
            messages: conversation turns, each {role, content}.
            namespace: target namespace (uses default if empty).

        Batch extraction uses PartialSuccessById semantics; when category=quota the error
        is retryable=false. (Distinct from cortrix_memory_extract_trigger, which triggers
        extraction for an existing session by session_id.)
        """
        ns = namespace or CORTRIX_NAMESPACE
        body = {"namespace": ns, "messages": messages}
        return request("POST", "/memory/extract", json_body=body, timeout=60.0)

    @mcp.tool()
    def cortrix_task_status(task_id: str) -> dict:
        """Query async upload task progress (F42; GET /documents/tasks/{task_id}/progress)."""
        return request("GET", f"/documents/tasks/{task_id}/progress", timeout=10.0)

    @mcp.tool()
    def cortrix_cancel_task(task_id: str) -> dict:
        """Cancel an async upload task (F42; DELETE /documents/tasks/{task_id}).

        Returns 409 (passed through) if the task already finished and cannot be cancelled.
        """
        return request("DELETE", f"/documents/tasks/{task_id}", timeout=10.0)

    @mcp.tool()
    def cortrix_query_explain(
        query: str,
        top_k: int = 5,
        namespaces: Optional[list[str]] = None,
        rerank: bool = True,
    ) -> dict:
        """Semantic search with explain enabled (POST /query?explain=true).

        Surfaces Class-A data-integrity fields (coverage_ratio / namespaces_failed) and the
        Class-B LLM-path explain section (ARCH section 1.6 phased rollout) into
        structured_data, so an agent can reason about retrieval quality.
        """
        ns = namespaces if namespaces else [CORTRIX_NAMESPACE]
        body = {"query": query, "namespaces": ns, "top_k": top_k, "rerank": rerank}
        return request("POST", "/query", json_body=body, params={"explain": "true"})
