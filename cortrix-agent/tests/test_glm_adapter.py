"""Unit tests for GLM adapter — SSE parsing, error handling, config."""

import json
import pytest
from unittest.mock import AsyncMock, MagicMock, patch

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent))

from llm.glm_adapter import GLMAdapter


@pytest.fixture
def adapter():
    return GLMAdapter(api_key="test-key-123", model="glm-4-flash")


@pytest.fixture
def paid_adapter():
    return GLMAdapter(api_key="test-key-456", model="glm-4-plus")


class TestGLMAdapterInit:
    """Test adapter initialization and configuration."""

    def test_default_model(self, adapter):
        assert adapter.model == "glm-4-flash"

    def test_paid_model(self, paid_adapter):
        assert paid_adapter.model == "glm-4-plus"

    def test_default_base_url(self, adapter):
        assert adapter.base_url == "https://open.bigmodel.cn/api/paas/v4"

    def test_custom_base_url(self):
        a = GLMAdapter(api_key="k", base_url="http://private-glm:8080/v4/")
        assert a.base_url == "http://private-glm:8080/v4"  # trailing slash stripped

    def test_auth_header(self, adapter):
        headers = adapter._client.headers
        assert headers["Authorization"] == "Bearer test-key-123"


class TestGLMStreamChat:
    """Test SSE streaming response parsing."""

    @pytest.mark.asyncio
    async def test_stream_chat_parses_sse(self, adapter):
        """Simulate GLM SSE response and verify chunk parsing."""
        sse_lines = [
            'data: {"choices":[{"delta":{"content":"Hello"}}]}',
            'data: {"choices":[{"delta":{"content":" world"}}]}',
            'data: [DONE]',
        ]

        mock_response = AsyncMock()
        mock_response.raise_for_status = MagicMock()
        mock_response.aiter_lines = _async_iter(sse_lines)

        mock_stream = AsyncMock()
        mock_stream.__aenter__ = AsyncMock(return_value=mock_response)
        mock_stream.__aexit__ = AsyncMock(return_value=False)

        with patch.object(adapter._client, 'stream', return_value=mock_stream):
            chunks = []
            async for chunk in adapter.stream_chat("system", "hi"):
                chunks.append(chunk)

        assert chunks == ["Hello", " world"]

    @pytest.mark.asyncio
    async def test_stream_chat_skips_empty_delta(self, adapter):
        """Lines with empty content should be skipped."""
        sse_lines = [
            'data: {"choices":[{"delta":{}}]}',
            'data: {"choices":[{"delta":{"content":"ok"}}]}',
            'data: [DONE]',
        ]

        mock_response = AsyncMock()
        mock_response.raise_for_status = MagicMock()
        mock_response.aiter_lines = _async_iter(sse_lines)

        mock_stream = AsyncMock()
        mock_stream.__aenter__ = AsyncMock(return_value=mock_response)
        mock_stream.__aexit__ = AsyncMock(return_value=False)

        with patch.object(adapter._client, 'stream', return_value=mock_stream):
            chunks = []
            async for chunk in adapter.stream_chat("system", "hi"):
                chunks.append(chunk)

        assert chunks == ["ok"]

    @pytest.mark.asyncio
    async def test_stream_chat_skips_non_data_lines(self, adapter):
        """Non-data lines (comments, empty) should be ignored."""
        sse_lines = [
            ': keep-alive',
            '',
            'data: {"choices":[{"delta":{"content":"x"}}]}',
            'event: message',
            'data: [DONE]',
        ]

        mock_response = AsyncMock()
        mock_response.raise_for_status = MagicMock()
        mock_response.aiter_lines = _async_iter(sse_lines)

        mock_stream = AsyncMock()
        mock_stream.__aenter__ = AsyncMock(return_value=mock_response)
        mock_stream.__aexit__ = AsyncMock(return_value=False)

        with patch.object(adapter._client, 'stream', return_value=mock_stream):
            chunks = []
            async for chunk in adapter.stream_chat("system", "hi"):
                chunks.append(chunk)

        assert chunks == ["x"]

    @pytest.mark.asyncio
    async def test_stream_chat_handles_json_decode_error(self, adapter):
        """Malformed JSON should be silently skipped."""
        sse_lines = [
            'data: NOT_JSON',
            'data: {"choices":[{"delta":{"content":"ok"}}]}',
            'data: [DONE]',
        ]

        mock_response = AsyncMock()
        mock_response.raise_for_status = MagicMock()
        mock_response.aiter_lines = _async_iter(sse_lines)

        mock_stream = AsyncMock()
        mock_stream.__aenter__ = AsyncMock(return_value=mock_response)
        mock_stream.__aexit__ = AsyncMock(return_value=False)

        with patch.object(adapter._client, 'stream', return_value=mock_stream):
            chunks = []
            async for chunk in adapter.stream_chat("system", "hi"):
                chunks.append(chunk)

        assert chunks == ["ok"]


class TestGLMCheckConnection:
    """Test connection check."""

    @pytest.mark.asyncio
    async def test_check_connection_success(self, adapter):
        mock_resp = MagicMock()
        mock_resp.status_code = 200

        with patch.object(adapter._client, 'post', new_callable=AsyncMock, return_value=mock_resp):
            result = await adapter.check_connection()

        assert result is True

    @pytest.mark.asyncio
    async def test_check_connection_failure(self, adapter):
        with patch.object(adapter._client, 'post', new_callable=AsyncMock, side_effect=Exception("timeout")):
            result = await adapter.check_connection()

        assert result is False

    @pytest.mark.asyncio
    async def test_check_connection_non_200(self, adapter):
        mock_resp = MagicMock()
        mock_resp.status_code = 401

        with patch.object(adapter._client, 'post', new_callable=AsyncMock, return_value=mock_resp):
            result = await adapter.check_connection()

        assert result is False


class TestGLMModelTiers:
    """Test that different model tiers can be configured."""

    @pytest.mark.parametrize("model", [
        "glm-4-plus", "glm-4-0520", "glm-4", "glm-4-air",
        "glm-4-airx", "glm-4-long", "glm-4-flash", "glm-4-flashx",
    ])
    def test_model_configurable(self, model):
        a = GLMAdapter(api_key="k", model=model)
        assert a.model == model


def _async_iter(lines):
    """Create an async iterator factory from a list of strings."""
    async def _iter():
        for line in lines:
            yield line
    return _iter
