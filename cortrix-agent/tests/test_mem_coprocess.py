"""Unit tests for MemoryCoprocessor (agent <-> MEM co-processing, design section 13)."""

import pytest

from agent_core import MemoryCoprocessor


class _Memory:
    def __init__(self, fail: bool = False):
        self.calls: list[dict] = []
        self.fail = fail

    async def log(self, namespace, *, query, response, user_id, session_id=None):
        if self.fail:
            raise RuntimeError("cortrix-server down")
        self.calls.append(
            {
                "namespace": namespace,
                "query": query,
                "response": response,
                "user_id": user_id,
                "session_id": session_id,
            }
        )


class _Client:
    def __init__(self, fail: bool = False):
        self.memory = _Memory(fail=fail)


@pytest.mark.asyncio
async def test_record_turn_calls_memory_log():
    client = _Client()
    mem = MemoryCoprocessor(client, namespace="docs")
    await mem.record_turn("s1", "hello", "hi there", user_id="u1")
    assert len(client.memory.calls) == 1
    call = client.memory.calls[0]
    assert call["namespace"] == "docs"
    assert call["query"] == "hello"
    assert call["response"] == "hi there"
    assert call["user_id"] == "u1"
    assert call["session_id"] == "s1"


@pytest.mark.asyncio
async def test_record_turn_user_id_fallback():
    """memory isolation: a missing user_id falls back to a default subject (never sent empty)."""
    client = _Client()
    mem = MemoryCoprocessor(client)
    await mem.record_turn("s2", "q", "a", user_id=None)
    assert client.memory.calls[0]["user_id"] == "default"


@pytest.mark.asyncio
async def test_record_turn_namespace_override():
    client = _Client()
    mem = MemoryCoprocessor(client, namespace="default")
    await mem.record_turn("s3", "q", "a", user_id="u", namespace="legal")
    assert client.memory.calls[0]["namespace"] == "legal"


@pytest.mark.asyncio
async def test_record_turn_never_raises_on_failure():
    """the agent design: a logging failure must not propagate (chat must not be blocked)."""
    client = _Client(fail=True)
    mem = MemoryCoprocessor(client)
    assert await mem.record_turn("s4", "q", "a", user_id="u") is None


@pytest.mark.asyncio
async def test_record_turn_tolerates_broken_client():
    """A None/broken client is swallowed (standalone safety)."""
    mem = MemoryCoprocessor(None)
    assert await mem.record_turn("s5", "q", "a", user_id="u") is None
