"""Unit tests for the LLM adapters (stream_chat + check_connection)."""

import pytest

from llm import ClaudeAdapter, MockAdapter, OllamaAdapter, OpenAIAdapter


# --- Mock adapter (no external client) ---


@pytest.mark.asyncio
async def test_mock_adapter_streams_default():
    out = "".join([c async for c in MockAdapter(chunk_size=8, delay=0).stream_chat("sys", "hello")])
    assert "Cortrix" in out


@pytest.mark.asyncio
async def test_mock_adapter_keyword_template():
    out = "".join([c async for c in MockAdapter(delay=0).stream_chat("sys", "do a semantic_search please")])
    assert "privacy" in out.lower()


@pytest.mark.asyncio
async def test_mock_adapter_check_connection():
    assert await MockAdapter().check_connection() is True


# --- OpenAI adapter (AsyncOpenAI-shaped client) ---


class _Delta:
    def __init__(self, content):
        self.content = content


class _Choice:
    def __init__(self, content):
        self.delta = _Delta(content)


class _Chunk:
    def __init__(self, content):
        self.choices = [_Choice(content)]


@pytest.mark.asyncio
async def test_openai_adapter_stream():
    a = OpenAIAdapter(api_key="k", model="gpt-4o-mini")

    async def _stream():
        for c in ["a", "b", None]:
            yield _Chunk(c)

    class _Comp:
        async def create(self, **kw):
            return _stream()

    class _ChatNS:
        completions = _Comp()

    class _C:
        chat = _ChatNS()

    a.client = _C()
    out = "".join([c async for c in a.stream_chat("s", "u")])
    assert out == "ab"


@pytest.mark.asyncio
async def test_openai_check_connection():
    a = OpenAIAdapter(api_key="k", model="m")

    class _Models:
        async def list(self):
            return []

    class _C:
        models = _Models()

    a.client = _C()
    assert await a.check_connection() is True


# --- httpx-based adapters (Claude / Ollama): a fake streaming client ---


class _FakeStreamResp:
    def __init__(self, lines):
        self._lines = lines

    def raise_for_status(self):
        return None

    async def aiter_lines(self):
        for line in self._lines:
            yield line


class _FakeStreamCtx:
    def __init__(self, lines):
        self._lines = lines

    async def __aenter__(self):
        return _FakeStreamResp(self._lines)

    async def __aexit__(self, *args):
        return False


class _FakeHttpx:
    def __init__(self, lines, status=200):
        self._lines = lines
        self._status = status

    def stream(self, method, url, **kw):
        return _FakeStreamCtx(self._lines)

    async def post(self, url, **kw):
        class _R:
            status_code = self._status

        return _R()

    async def get(self, url, **kw):
        class _R:
            status_code = self._status

        return _R()


@pytest.mark.asyncio
async def test_claude_adapter_stream_stops_at_message_stop():
    a = ClaudeAdapter(api_key="k")
    a._client = _FakeHttpx(
        [
            'data: {"type":"content_block_delta","delta":{"text":"Hel"}}',
            'data: {"type":"content_block_delta","delta":{"text":"lo"}}',
            'data: {"type":"message_stop"}',
            'data: {"type":"content_block_delta","delta":{"text":"AFTER"}}',
        ]
    )
    out = "".join([c async for c in a.stream_chat("s", "u")])
    assert out == "Hello"


@pytest.mark.asyncio
async def test_claude_adapter_error_event_raises():
    a = ClaudeAdapter(api_key="k")
    a._client = _FakeHttpx(['data: {"type":"error","error":{"type":"overloaded","message":"busy"}}'])
    with pytest.raises(RuntimeError):
        [c async for c in a.stream_chat("s", "u")]


@pytest.mark.asyncio
async def test_claude_check_connection():
    a = ClaudeAdapter(api_key="k")
    a._client = _FakeHttpx([], status=200)
    assert await a.check_connection() is True


@pytest.mark.asyncio
async def test_ollama_adapter_stream_stops_at_done():
    a = OllamaAdapter()
    a._client = _FakeHttpx(
        [
            'data: {"choices":[{"delta":{"content":"Hi"}}]}',
            'data: {"choices":[{"delta":{"content":" there"}}]}',
            "data: [DONE]",
            'data: {"choices":[{"delta":{"content":"AFTER"}}]}',
        ]
    )
    out = "".join([c async for c in a.stream_chat("s", "u")])
    assert out == "Hi there"


@pytest.mark.asyncio
async def test_ollama_check_connection():
    a = OllamaAdapter()
    a._client = _FakeHttpx([], status=200)
    assert await a.check_connection() is True
