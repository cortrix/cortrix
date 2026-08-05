"""Integration tests for the chat / sessions / health / config routes (design section 9.1).

Driven through FastAPI's TestClient over an app assembled by ``main.build_app`` with a
stubbed RAG provider and a stubbed LLM (no cortrix-server, no real LLM).
"""

from __future__ import annotations

import json
from typing import AsyncIterator

import pytest
from fastapi.testclient import TestClient

import main
from agent_core import SessionStore
from agent_core.errors import AgentError
from agent_core.sdk_rag import RagChunk, RagResult


class _StubRag:
    def __init__(self, *, result=None, error=None):
        self._result = result if result is not None else _result_with_chunk()
        self._error = error

    async def retrieve(self, query, *, namespace=None) -> RagResult:
        if self._error is not None:
            raise self._error
        return self._result


class _StubLLM:
    def __init__(self, *, pieces=("Based on", " the docs"), error=None):
        self._pieces = pieces
        self._error = error

    async def stream_chat(self, system_prompt, user_message, temperature: float = 0.7) -> AsyncIterator[str]:
        if self._error is not None:
            raise self._error
        for p in self._pieces:
            yield p

    async def check_connection(self) -> bool:
        return True


def _result_with_chunk():
    return RagResult(
        chunks=[RagChunk(chunk_id="d1#0", source_path="/docs/a.pdf", content="ctx text", score=0.9)],
        latency_ms=5,
    )


def _client(*, rag=None, llm=None, store=None):
    store = store or SessionStore()
    app = main.build_app(
        rag_provider=rag or _StubRag(),
        llm_adapter=llm or _StubLLM(),
        session_store=store,
        model_name="mock-model",
    )
    return TestClient(app), store


def _parse_sse(text: str):
    """Return the list of JSON payloads from ``data:`` lines (ignoring [DONE])."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("data:"):
            payload = line[len("data:"):].strip()
            if payload and payload != "[DONE]":
                out.append(json.loads(payload))
    return out


def test_chat_sse_emits_chunks_then_meta_then_done():
    client, _ = _client()
    resp = client.post("/chat", json={"message": "find docs", "session_id": "s1"})
    assert resp.status_code == 200
    body = resp.text
    assert "[DONE]" in body
    payloads = _parse_sse(body)
    chunk_texts = [p["chunk"] for p in payloads if "chunk" in p]
    metas = [p["meta"] for p in payloads if "meta" in p]
    assert "".join(chunk_texts) == "Based on the docs"
    assert len(metas) == 1
    meta = metas[0]
    # A-class always present.
    assert meta["session_id"] == "s1"
    assert meta["chunk_ids"] == ["d1#0"]
    assert meta["rag_status"] == "success"
    assert meta["tenant_id"] is None
    # internal field must not leak.
    assert "_assistant_text" not in meta


def test_explain_true_exposes_b_class_meta():
    client, _ = _client()
    resp = client.post("/chat?explain=true", json={"message": "q", "session_id": "s1"})
    meta = [p["meta"] for p in _parse_sse(resp.text) if "meta" in p][0]
    assert "prompt_text" in meta
    assert meta["model_used"] == "mock-model"
    assert meta["rag_chunks_full"][0]["chunk_id"] == "d1#0"


def test_explain_absent_by_default():
    client, _ = _client()
    resp = client.post("/chat", json={"message": "q", "session_id": "s1"})
    meta = [p["meta"] for p in _parse_sse(resp.text) if "meta" in p][0]
    assert "prompt_text" not in meta
    assert "model_used" not in meta


def test_tenant_header_echoed_in_meta_and_session():
    client, store = _client()
    resp = client.post(
        "/chat",
        json={"message": "q", "session_id": "s1"},
        headers={"X-Cortrix-Tenant-Id": "tenant-9"},
    )
    meta = [p["meta"] for p in _parse_sse(resp.text) if "meta" in p][0]
    assert meta["tenant_id"] == "tenant-9"
    assert store.get_history("s1")["tenant_id"] == "tenant-9"


def test_chat_writes_back_session_turn():
    client, store = _client()
    client.post("/chat", json={"message": "hello there", "session_id": "s1"})
    msgs = store.get_history("s1")["messages"]
    assert len(msgs) == 2
    assert msgs[0]["content"] == "hello there"
    assert msgs[1]["content"] == "Based on the docs"


def test_namespace_header_forwarded_to_rag():
    captured = {}

    class _NsRag(_StubRag):
        async def retrieve(self, query, *, namespace=None):
            captured["ns"] = namespace
            return _result_with_chunk()

    client, _ = _client(rag=_NsRag())
    client.post(
        "/chat",
        json={"message": "q", "session_id": "s1"},
        headers={"X-Cortrix-Namespace": "ns-custom"},
    )
    assert captured["ns"] == "ns-custom"


def test_l3_failure_emits_four_field_error_event():
    """RAG + LLM both down -> executor raises AgentError -> SSE error event with the
    GEN-Agent 4-field envelope (design section 9.3)."""
    rag = _StubRag(error=RuntimeError("rag down"))
    llm = _StubLLM(error=RuntimeError("llm down"))
    client, _ = _client(rag=rag, llm=llm)
    resp = client.post("/chat?debug=true", json={"message": "q", "session_id": "s1"})
    assert resp.status_code == 200  # SSE stream still 200; error carried in-band
    payloads = _parse_sse(resp.text)
    errors = [p["error"] for p in payloads if "error" in p]
    assert len(errors) == 1
    err = errors[0]
    assert err["code"] == "CX_ERR_AGENT_RAG_FAILED"
    assert set(err.keys()) == {
        "code",
        "message",
        "retryable",
        "category",
        "retry_after_ms",
        "structured_data",
    }


def test_chat_rejects_empty_message():
    client, _ = _client()
    resp = client.post("/chat", json={"message": "", "session_id": "s1"})
    assert resp.status_code == 422  # pydantic min_length
    # R10 GAP-E2: validation errors are wrapped in the GEN-Agent envelope (not the
    # raw FastAPI {"detail": [...]} array) so an Agent can parse them like any other
    # CX_ERR response.
    body = resp.json()
    assert "error" in body, body
    err = body["error"]
    assert err["code"] == "CX_ERR_AGENT_VALIDATION_ERROR"
    assert err["retryable"] is False
    assert err["category"] == "permanent"
    assert "validation_errors" in err["structured_data"]


def test_chat_missing_message_field_is_agent_friendly():
    """A body missing `message` entirely also gets the CX_ERR envelope (GAP-E2)."""
    client, _ = _client()
    resp = client.post("/chat", json={"session_id": "s1"})
    assert resp.status_code == 422
    err = resp.json()["error"]
    assert err["code"] == "CX_ERR_AGENT_VALIDATION_ERROR"
    assert err["structured_data"]["validation_errors"]


def test_get_session_returns_history():
    client, store = _client()
    client.post("/chat", json={"message": "q1", "session_id": "s1"})
    resp = client.get("/sessions/s1")
    assert resp.status_code == 200
    data = resp.json()
    assert data["window_size"] == 10
    assert len(data["messages"]) == 2
    assert data["tenant_id"] is None


def test_get_missing_session_returns_404_four_field_error():
    client, _ = _client()
    resp = client.get("/sessions/does-not-exist")
    assert resp.status_code == 404
    err = resp.json()["error"]
    assert err["code"] == "CX_ERR_AGENT_SESSION_NOT_FOUND"
    assert err["retryable"] is False
    assert err["structured_data"]["session_id"] == "does-not-exist"


def test_health_reports_status_and_llm_reachable():
    client, _ = _client()
    data = client.get("/health").json()
    assert data["status"] == "ready"
    assert "cortrix_server" in data
    assert data["llm_reachable"] is True


def test_config_masks_api_key():
    client, _ = _client()
    data = client.get("/config").json()
    assert "api_key_masked" in data
    # mock provider -> no key -> masked is None; never a raw key field.
    assert "api_key" not in data


def test_config_providers_lists_six_with_deepseek():
    client, _ = _client()
    providers = client.get("/config/providers").json()["providers"]
    ids = {p["id"] for p in providers}
    assert {"openai", "claude", "ollama", "glm", "mock", "deepseek"} <= ids


def test_put_agent_llm_forwards_to_cortrix_server():
    # D3.5 r2 · Wave P · P4: PUT /config/agent_llm forwards to the cortrix-server
    # admin endpoint and returns its (masked) response. Override the forwarder so
    # the test does not hit a live server, and assert the payload is forwarded with
    # None fields stripped (server-side merge semantics).
    from routes import config as config_routes

    captured: dict = {}

    async def _fake_forward(payload):
        captured.update(payload)
        return {"provider": "claude", "api_key": "sk-c...", "model": "claude-x"}

    client, _ = _client()
    client.app.dependency_overrides[config_routes.get_agent_llm_forwarder] = (
        lambda: _fake_forward
    )
    resp = client.put("/config/agent_llm",
                      json={"provider": "claude", "api_key": "sk-claude-123"})
    assert resp.status_code == 200
    assert resp.json()["provider"] == "claude"
    assert captured == {"provider": "claude", "api_key": "sk-claude-123"}


def test_put_agent_llm_502_on_forward_failure():
    from routes import config as config_routes

    async def _boom(_payload):
        raise RuntimeError("connection refused")

    client, _ = _client()
    client.app.dependency_overrides[config_routes.get_agent_llm_forwarder] = (
        lambda: _boom
    )
    resp = client.put("/config/agent_llm", json={"provider": "claude"})
    assert resp.status_code == 502
