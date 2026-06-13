"""Unit tests for the DeepSeek LLM adapter (OpenAI-compatible, design section 7.2)."""

import pytest

from llm import DeepSeekAdapter


def test_deepseek_defaults():
    adapter = DeepSeekAdapter(api_key="sk-test")
    assert adapter.model == "deepseek-chat"
    assert "deepseek.com" in str(adapter.client.base_url)


def test_deepseek_custom_model_and_url():
    adapter = DeepSeekAdapter(
        api_key="k", model="deepseek-reasoner", base_url="https://proxy.example/v1"
    )
    assert adapter.model == "deepseek-reasoner"
    assert "proxy.example" in str(adapter.client.base_url)


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
async def test_deepseek_stream_yields_content():
    """stream_chat yields delta.content and skips empty/None deltas."""
    adapter = DeepSeekAdapter(api_key="sk-test")

    async def _fake_stream():
        for c in ["Hel", "lo", None, " Cortrix"]:
            yield _Chunk(c)

    class _Completions:
        async def create(self, **kwargs):
            assert kwargs["stream"] is True
            return _fake_stream()

    class _Chat:
        completions = _Completions()

    class _FakeClient:
        chat = _Chat()

    adapter.client = _FakeClient()

    out = [piece async for piece in adapter.stream_chat("sys", "hi")]
    assert "".join(out) == "Hello Cortrix"


@pytest.mark.asyncio
async def test_deepseek_check_connection_ok():
    adapter = DeepSeekAdapter(api_key="sk-test")

    class _Models:
        async def list(self):
            return ["deepseek-chat"]

    class _OkClient:
        models = _Models()

    adapter.client = _OkClient()
    assert await adapter.check_connection() is True


@pytest.mark.asyncio
async def test_deepseek_check_connection_failure():
    adapter = DeepSeekAdapter(api_key="sk-test")

    class _Models:
        async def list(self):
            raise RuntimeError("network down")

    class _BadClient:
        models = _Models()

    adapter.client = _BadClient()
    assert await adapter.check_connection() is False
