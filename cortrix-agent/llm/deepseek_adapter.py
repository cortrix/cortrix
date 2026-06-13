"""DeepSeek LLM adapter (OpenAI-compatible API).

DeepSeek (added in Wave E — design section 7.2) exposes an OpenAI-compatible
chat-completions API, so this adapter reuses the OpenAI async client with DeepSeek's
base_url and default model. It is a national provider for the China market (alongside
GLM), giving the Cortrix Agent a 6th LLM option.
"""

from typing import AsyncIterator

from openai import AsyncOpenAI

from .base import BaseLLMAdapter


class DeepSeekAdapter(BaseLLMAdapter):

    def __init__(
        self,
        api_key: str,
        model: str = "deepseek-chat",
        base_url: str = "https://api.deepseek.com/v1",
    ):
        self.client = AsyncOpenAI(api_key=api_key, base_url=base_url)
        self.model = model

    async def close(self) -> None:
        """Close the underlying HTTP client."""
        await self.client.close()

    async def stream_chat(
        self,
        system_prompt: str,
        user_message: str,
        temperature: float = 0.7,
    ) -> AsyncIterator[str]:
        stream = await self.client.chat.completions.create(
            model=self.model,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_message},
            ],
            temperature=temperature,
            stream=True,
        )
        async for chunk in stream:
            delta = chunk.choices[0].delta
            if delta.content:
                yield delta.content

    async def check_connection(self) -> bool:
        try:
            await self.client.models.list()
            return True
        except Exception:
            return False
