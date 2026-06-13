"""Abstract base class for LLM adapters."""

from abc import ABC, abstractmethod
from typing import AsyncIterator


class BaseLLMAdapter(ABC):

    @abstractmethod
    async def stream_chat(
        self,
        system_prompt: str,
        user_message: str,
        temperature: float = 0.7,
    ) -> AsyncIterator[str]:
        """Stream chat completion chunks."""
        ...

    @abstractmethod
    async def check_connection(self) -> bool:
        """Check if LLM API is reachable."""
        ...
