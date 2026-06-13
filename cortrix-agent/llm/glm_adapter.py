"""GLM (Zhipu AI) LLM adapter using OpenAI-compatible API.

Supported models (highest to lowest tier):
  - glm-4-plus     : top flagship model, best quality
  - glm-4-0520     : high-performance version
  - glm-4          : standard version, good value
  - glm-4-air      : lightweight version, fast
  - glm-4-airx     : lightweight accelerated version
  - glm-4-long     : long-context version (128K tokens)
  - glm-4-flash    : free version, good for dev/testing
  - glm-4-flashx   : free accelerated version

Uses a shared httpx.AsyncClient for connection pooling and efficiency.
"""

import json
from typing import AsyncIterator
import httpx
from .base import BaseLLMAdapter


class GLMAdapter(BaseLLMAdapter):

    def __init__(
        self,
        api_key: str,
        model: str = "glm-4-flash",
        base_url: str = "https://open.bigmodel.cn/api/paas/v4",
    ):
        self.api_key = api_key
        self.model = model
        self.base_url = base_url.rstrip("/")
        self._client = httpx.AsyncClient(
            timeout=httpx.Timeout(60.0, connect=10.0),
            headers={
                "Authorization": f"Bearer {self.api_key}",
                "Content-Type": "application/json",
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
        url = f"{self.base_url}/chat/completions"
        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_message},
            ],
            "temperature": temperature,
            "stream": True,
        }

        async with self._client.stream("POST", url, json=payload) as resp:
            resp.raise_for_status()
            async for line in resp.aiter_lines():
                if not line.startswith("data: "):
                    continue
                data = line[len("data: "):]
                if data == "[DONE]":
                    break
                try:
                    chunk = json.loads(data)
                except json.JSONDecodeError:
                    continue
                delta = chunk.get("choices", [{}])[0].get("delta", {})
                content = delta.get("content")
                if content:
                    yield content

    async def check_connection(self) -> bool:
        try:
            url = f"{self.base_url}/chat/completions"
            payload = {
                "model": self.model,
                "messages": [{"role": "user", "content": "hi"}],
                "max_tokens": 1,
            }
            resp = await self._client.post(url, json=payload)
            return resp.status_code == 200
        except Exception:
            return False
