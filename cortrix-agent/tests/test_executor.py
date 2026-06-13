"""Unit tests for ChatExecutor RAG degradation L1/L2/L3 (design section 9.2).

The executor is exercised against a stubbed SdkRagProvider and a stubbed LLM adapter
(standalone D3 discipline — cortrix-server is not running). Backoff sleeps are patched
to zero so the N=3 retry path runs fast.
"""

from __future__ import annotations

from typing import AsyncIterator

import pytest

import agent_core.executor as executor_mod
from agent_core import ChatContext, ChatExecutor
from agent_core.errors import AgentError
from agent_core.explain import RAG_STATUS_DEGRADED, RAG_STATUS_SUCCESS
from agent_core.sdk_rag import RagChunk, RagResult


@pytest.fixture(autouse=True)
def _no_backoff(monkeypatch):
    """Make the L1 retry backoff instant."""

    async def _instant(_seconds):
        return None

    monkeypatch.setattr(executor_mod.asyncio, "sleep", _instant)


class StubRag:
    """A stand-in for SdkRagProvider.retrieve with scriptable behaviour."""

    def __init__(self, *, result=None, fail_times=0, error=RuntimeError("rag down")):
        self._result = result
        self._fail_times = fail_times
        self._error = error
        self.calls = 0
        self.namespaces: list = []

    async def retrieve(self, query: str, *, namespace=None) -> RagResult:
        self.calls += 1
        self.namespaces.append(namespace)
        if self.calls <= self._fail_times:
            raise self._error
        return self._result if self._result is not None else RagResult()


class StubLLM:
    """A stand-in LLM adapter: streams fixed pieces or raises."""

    def __init__(self, *, pieces=("Hello", " world"), error=None):
        self._pieces = pieces
        self._error = error
        self.prompts: list = []

    async def stream_chat(self, system_prompt: str, user_message: str, temperature: float = 0.7) -> AsyncIterator[str]:
        self.prompts.append(system_prompt)
        if self._error is not None:
            raise self._error
        for p in self._pieces:
            yield p


def _result_with_chunks():
    return RagResult(
        chunks=[RagChunk(chunk_id="d1#0", source_path="/docs/a.pdf", content="ctx", score=0.9)],
        latency_ms=5,
    )


async def _drain(executor, message, context):
    """Collect (chunks_text, meta) from an executor run."""
    chunks = []
    meta = None
    async for ev in executor.execute(message, context):
        if ev.kind == "chunk":
            chunks.append(ev.chunk)
        elif ev.kind == "meta":
            meta = ev.meta
    return "".join(chunks), meta


@pytest.mark.asyncio
async def test_happy_path_success_status_and_chunk_ids():
    rag = StubRag(result=_result_with_chunks())
    llm = StubLLM(pieces=("Hi", "!"))
    ex = ChatExecutor(rag, llm, model_name="mock-model")
    text, meta = await _drain(ex, "q", ChatContext(session_id="s1"))
    assert text == "Hi!"
    assert meta["rag_status"] == RAG_STATUS_SUCCESS
    assert meta["chunk_ids"] == ["d1#0"]
    assert meta["chunks_used"] == 1
    assert meta["_assistant_text"] == "Hi!"
    assert rag.calls == 1  # no retry needed


@pytest.mark.asyncio
async def test_l1_retries_then_succeeds_on_third_attempt():
    """L1: 2 transient failures then success -> 3 total attempts, success status."""
    rag = StubRag(result=_result_with_chunks(), fail_times=2)
    ex = ChatExecutor(rag, StubLLM(), model_name="m")
    text, meta = await _drain(ex, "q", ChatContext(session_id="s1"))
    assert rag.calls == 3
    assert meta["rag_status"] == RAG_STATUS_SUCCESS
    assert "Hello world" == text


@pytest.mark.asyncio
async def test_l2_llm_only_fallback_when_rag_exhausted():
    """L2: RAG fails all N=3 -> degrade, LLM answers with empty context."""
    rag = StubRag(fail_times=99)  # always fails
    llm = StubLLM(pieces=("Answer", " without", " RAG"))
    ex = ChatExecutor(rag, llm, model_name="m")
    text, meta = await _drain(ex, "q", ChatContext(session_id="s1", debug=True))
    assert rag.calls == 3  # capped at N=3
    assert meta["rag_status"] == RAG_STATUS_DEGRADED
    assert meta["chunks_used"] == 0
    assert text == "Answer without RAG"
    # C-class failure detail present because debug=True.
    assert "rag_call_failed_detail" in meta
    assert "cortrix_server_error" in meta["rag_call_failed_detail"]


@pytest.mark.asyncio
async def test_l3_hard_error_when_rag_and_llm_both_fail():
    """L3: RAG exhausted (degraded) AND the LLM also fails -> AgentError raised."""
    rag = StubRag(fail_times=99)
    llm = StubLLM(error=RuntimeError("llm dead"))
    ex = ChatExecutor(rag, llm, model_name="m")
    with pytest.raises(AgentError) as exc_info:
        await _drain(ex, "q", ChatContext(session_id="s1"))
    err = exc_info.value
    assert err.code == "CX_ERR_F48_RAG_FAILED"
    assert err.retryable is False
    assert err.structured_data.get("fallback_attempted") is True
    assert "llm_error" in err.structured_data


@pytest.mark.asyncio
async def test_llm_fails_but_rag_succeeded_degrades_to_raw_chunks():
    """RAG ok but LLM fails -> not L3; degrade to returning retrieved chunks."""
    rag = StubRag(result=_result_with_chunks())
    llm = StubLLM(error=RuntimeError("llm dead"))
    ex = ChatExecutor(rag, llm, model_name="m")
    text, meta = await _drain(ex, "q", ChatContext(session_id="s1"))
    assert meta["rag_status"] == RAG_STATUS_DEGRADED
    assert "/docs/a.pdf" in text  # raw chunk surfaced to the user
    assert "ctx" in text


@pytest.mark.asyncio
async def test_namespace_override_passed_to_rag():
    rag = StubRag(result=RagResult())
    ex = ChatExecutor(rag, StubLLM(), model_name="m")
    await _drain(ex, "q", ChatContext(session_id="s1", namespace="ns-override"))
    assert rag.namespaces == ["ns-override"]


@pytest.mark.asyncio
async def test_explain_meta_includes_prompt_and_model():
    rag = StubRag(result=_result_with_chunks())
    ex = ChatExecutor(rag, StubLLM(), model_name="claude-x")
    _, meta = await _drain(ex, "q", ChatContext(session_id="s1", explain=True))
    assert meta["model_used"] == "claude-x"
    assert "prompt_text" in meta
    assert meta["rag_chunks_full"][0]["chunk_id"] == "d1#0"
