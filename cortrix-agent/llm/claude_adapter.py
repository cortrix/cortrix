"""Anthropic Claude LLM adapter using the Messages API with SSE streaming."""

from __future__ import annotations

from typing import AsyncIterator, Optional
import httpx
import json
from .base import BaseLLMAdapter


class ClaudeAdapter(BaseLLMAdapter):
    """Adapter for Anthropic's Claude API (Messages API with streaming).

    Uses a shared httpx.AsyncClient for connection pooling and efficiency.
    The client must be closed explicitly via ``close()`` or by using the
    adapter inside an async context that calls ``close()`` on teardown.
    """

    DEFAULT_MODEL = "claude-sonnet-4-5-20250929"
    API_URL = "https://api.anthropic.com/v1/messages"
    API_VERSION = "2023-06-01"

    def __init__(
        self,
        api_key: str,
        model: Optional[str] = None,
        max_tokens: int = 4096,
    ):
        self.api_key = api_key
        self.model = model or self.DEFAULT_MODEL
        self.max_tokens = max_tokens
        self._client = httpx.AsyncClient(
            timeout=httpx.Timeout(120.0, connect=10.0),
            headers={
                "x-api-key": self.api_key,
                "anthropic-version": self.API_VERSION,
                "content-type": "application/json",
            },
        )

    async def close(self) -> None:
        """Close the underlying HTTP client."""
        await self._client.aclose()

    async def stream_chat(
        self,
        system_prompt: str,
        user_message: str,
        temperature: float = 0.7,
    ) -> AsyncIterator[str]:
        """Stream chat completion chunks via the Anthropic Messages API.

        The Anthropic streaming format sends server-sent events with types:
          - message_start: contains the initial message metadata
          - content_block_start: signals the start of a content block
          - content_block_delta: contains incremental text (delta.text)
          - content_block_stop: signals the end of a content block
          - message_delta: contains stop_reason and usage
          - message_stop: signals the end of the message

        We extract text from content_block_delta events and yield it.
        """
        payload = {
            "model": self.model,
            "max_tokens": self.max_tokens,
            "system": system_prompt,
            "messages": [
                {"role": "user", "content": user_message},
            ],
            "temperature": temperature,
            "stream": True,
        }

        async with self._client.stream(
            "POST", self.API_URL, json=payload
        ) as resp:
            resp.raise_for_status()
            async for line in resp.aiter_lines():
                if not line.startswith("data: "):
                    continue
                data_str = line[len("data: "):]

                try:
                    event = json.loads(data_str)
                except json.JSONDecodeError:
                    continue

                event_type = event.get("type", "")

                if event_type == "content_block_delta":
                    delta = event.get("delta", {})
                    text = delta.get("text", "")
                    if text:
                        yield text

                elif event_type == "message_stop":
                    break

                elif event_type == "error":
                    error_info = event.get("error", {})
                    raise RuntimeError(
                        f"Claude API error: {error_info.get('type', 'unknown')} "
                        f"- {error_info.get('message', 'no details')}"
                    )

    async def check_connection(self) -> bool:
        """Check if the Anthropic API is reachable by sending a minimal request."""
        payload = {
            "model": self.model,
            "max_tokens": 1,
            "messages": [{"role": "user", "content": "hi"}],
        }

        try:
            resp = await self._client.post(self.API_URL, json=payload)
            return resp.status_code == 200
        except Exception:
            return False
