"""Unit tests for the A/B/C response-meta tiers (design section 1.7 / P-4)."""

from __future__ import annotations

from agent_core.explain import (
    RAG_STATUS_DEGRADED,
    RAG_STATUS_SUCCESS,
    build_response_meta,
)

_BASE = dict(
    session_id="s1",
    tenant_id=None,
    chunk_ids=["d1#0", "d1#1"],
    latency_ms={"rag": 5, "llm": 100, "total": 110},
    rag_status=RAG_STATUS_SUCCESS,
)

# Fields that must NEVER appear unless explain/debug gate them.
_B_FIELDS = {"prompt_text", "rag_chunks_full", "model_used", "llm_token_count"}
_C_FIELDS = {"rag_call_failed_detail", "llm_call_failed_detail"}


def test_a_class_always_present():
    """A-class fields are returned even with explain=debug=False."""
    meta = build_response_meta(**_BASE)
    for key in ("session_id", "tenant_id", "chunks_used", "chunk_ids", "latency_ms", "rag_status"):
        assert key in meta
    assert meta["chunks_used"] == 2  # derived from len(chunk_ids)
    # tenant_id is A-class but null-allowed in CE.
    assert meta["tenant_id"] is None


def test_b_and_c_absent_without_flags():
    meta = build_response_meta(**_BASE)
    assert _B_FIELDS.isdisjoint(meta.keys())
    assert _C_FIELDS.isdisjoint(meta.keys())


def test_b_class_only_under_explain():
    meta = build_response_meta(
        **_BASE,
        explain=True,
        prompt_text="PROMPT",
        rag_chunks_full=[{"chunk_id": "d1#0", "content": "x", "score": 0.9}],
        model_used="claude-sonnet-4-5",
        llm_token_count={"input": 10, "output": 5},
    )
    assert _B_FIELDS.issubset(meta.keys())
    assert meta["prompt_text"] == "PROMPT"
    assert meta["model_used"] == "claude-sonnet-4-5"
    # C-class still absent (no debug).
    assert _C_FIELDS.isdisjoint(meta.keys())


def test_c_class_only_under_debug_on_failure():
    meta = build_response_meta(
        **{**_BASE, "rag_status": RAG_STATUS_DEGRADED},
        debug=True,
        rag_call_failed_detail={"cortrix_server_error": "timeout"},
    )
    assert "rag_call_failed_detail" in meta
    assert meta["rag_call_failed_detail"] == {"cortrix_server_error": "timeout"}
    # llm detail not provided -> not emitted even with debug on.
    assert "llm_call_failed_detail" not in meta
    # B-class still absent (no explain).
    assert _B_FIELDS.isdisjoint(meta.keys())


def test_debug_without_failure_detail_emits_no_c_fields():
    meta = build_response_meta(**_BASE, debug=True)
    assert _C_FIELDS.isdisjoint(meta.keys())


def test_tenant_id_echoed_when_present():
    meta = build_response_meta(**{**_BASE, "tenant_id": "tenant-42"})
    assert meta["tenant_id"] == "tenant-42"
