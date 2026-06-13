"""Mock LLM adapter for testing without real LLM API."""

import asyncio
from typing import AsyncIterator
from .base import BaseLLMAdapter

# Template responses keyed by demo flow keywords
_MOCK_RESPONSES = {
    "semantic_search": (
        "Based on the retrieved documents:\n\n"
        "1. [/contracts/privacy_policy.pdf]: Data privacy clauses specify...\n"
        "2. [/manuals/gdpr_guide.pdf]: GDPR compliance requirements include...\n\n"
        "These documents detail the data privacy protection measures."
    ),
    "memory": (
        "Based on our previous conversation, you asked about data privacy documents. "
        "I found privacy_policy.pdf and gdpr_guide.pdf for you. "
        "The key difference is their scope of application."
    ),
    "structured_query": (
        "Database query result:\n"
        "Monthly sales orders: 156\n\n"
        "Related documents:\n"
        "- [/reports/sales_monthly.pdf]: February sales grew year-over-year...\n"
        "- [/contracts/top_clients.xlsx]: Key client order distribution...\n\n"
        "Analysis: Growth driven by existing customer repurchases."
    ),
    "default": (
        "I found relevant information from Cortrix semantic storage. "
        "Based on the retrieved context, here is my analysis of your query."
    ),
}


class MockAdapter(BaseLLMAdapter):
    """Mock adapter that returns template responses for testing."""

    def __init__(self, chunk_size: int = 8, delay: float = 0.02):
        self.chunk_size = chunk_size
        self.delay = delay

    async def stream_chat(
        self,
        system_prompt: str,
        user_message: str,
        temperature: float = 0.7,
    ) -> AsyncIterator[str]:
        # Pick response template based on keywords in user_message
        response = _MOCK_RESPONSES["default"]
        for key in ("semantic_search", "memory", "structured_query"):
            if key in user_message.lower() or key.replace("_", " ") in user_message.lower():
                response = _MOCK_RESPONSES[key]
                break

        # Stream in chunks to simulate real LLM behavior
        for i in range(0, len(response), self.chunk_size):
            chunk = response[i : i + self.chunk_size]
            if self.delay > 0:
                await asyncio.sleep(self.delay)
            yield chunk

    async def check_connection(self) -> bool:
        return True
