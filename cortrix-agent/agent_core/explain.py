"""Response meta builder: the A/B/C field tiers + ?explain mode (design section 1.7 / P-4).

Agent is the 7th evidence Feature for the project-level observability principle
(ARCH section 1.6). Chat response meta is split into three tiers:

  * A (data integrity)  — ALWAYS returned. session_id / chunks_used / chunk_ids /
    latency_ms / rag_status. Plus ``tenant_id``: A-class, always present, value ``None``
    in the CE single-tenant environment (design section 1.7 V3 decision 6). A-class
    fields other than tenant_id must NOT be null.
  * B (LLM dependency path) — exposed only when ``explain=true``. prompt_text /
    rag_chunks_full / model_used / llm_token_count.
  * C (anomaly debug) — emitted only on failure when debug is enabled. *_call_failed_detail.

This module assembles the ``meta`` object streamed as the final ``data:{meta}`` SSE
event before ``data:[DONE]`` (design section 9.1).
"""

from __future__ import annotations

from typing import Any, Optional

# rag_status enum (design section 9.1 / P-6): success / degraded / failed.
RAG_STATUS_SUCCESS = "success"
RAG_STATUS_DEGRADED = "degraded"
RAG_STATUS_FAILED = "failed"


def build_response_meta(
    *,
    session_id: str,
    tenant_id: Optional[str],
    chunk_ids: list[str],
    latency_ms: dict[str, int],
    rag_status: str,
    explain: bool = False,
    debug: bool = False,
    # B-class (explain mode) inputs:
    prompt_text: Optional[str] = None,
    rag_chunks_full: Optional[list[dict[str, Any]]] = None,
    model_used: Optional[str] = None,
    llm_token_count: Optional[dict[str, int]] = None,
    # C-class (debug-on-failure) inputs:
    rag_call_failed_detail: Optional[dict[str, Any]] = None,
    llm_call_failed_detail: Optional[dict[str, Any]] = None,
) -> dict[str, Any]:
    """Assemble the chat ``meta`` object honoring the A/B/C tiers (design section 1.7).

    A-class fields are always present. B-class fields appear only when ``explain`` is
    True. C-class ``*_call_failed_detail`` fields appear only when ``debug`` is True and
    the corresponding detail is provided.
    """
    # --- A class: always present (data integrity) ---
    meta: dict[str, Any] = {
        "session_id": session_id,
        # A-class but null-allowed ONLY for tenant_id in CE (design section 1.7).
        "tenant_id": tenant_id,
        "chunks_used": len(chunk_ids),
        "chunk_ids": chunk_ids,
        "latency_ms": latency_ms,
        "rag_status": rag_status,
    }

    # A-class citations: the lightweight per-source provenance the chat UI needs to
    # render its "sources" list — source name + score + a short content snippet. The
    # full chunk text and the prompt stay B-class (explain). Built from the same
    # rag_chunks_full the executor always supplies (independent of explain); empty when
    # nothing was retrieved. Without this the UI could only show bare chunk_ids, so each
    # source rendered blank with a 0.000 score.
    meta["citations"] = [
        {
            "chunk_id": c.get("chunk_id"),
            "source_path": c.get("source_path"),
            "score": c.get("score"),
            "snippet": str(c.get("content") or "")[:240],
        }
        for c in (rag_chunks_full or [])
    ]

    # --- B class: LLM dependency path, exposed under ?explain=true ---
    if explain:
        meta["prompt_text"] = prompt_text
        meta["rag_chunks_full"] = rag_chunks_full or []
        meta["model_used"] = model_used
        meta["llm_token_count"] = llm_token_count or {}

    # --- C class: anomaly debug, only on failure + debug enabled ---
    if debug:
        if rag_call_failed_detail is not None:
            meta["rag_call_failed_detail"] = rag_call_failed_detail
        if llm_call_failed_detail is not None:
            meta["llm_call_failed_detail"] = llm_call_failed_detail

    return meta
