"""End-to-end round-trip tests — integration DEFERRED (skeleton only).

These exercise the real framework + real LLM + a live cortrix-server (feature
design sections 10.2 / 10.3 / 5.4-bis e2e). They are intentionally **skipped**
in standalone development: they need network, API keys, and the installed
frameworks, and the integration wiring lands at the quick start release gate. The
standalone unit suite (``tests/test_*.py``) covers the conversion + error
contract with mocks.

To enable later, set ``CORTRIX_SKILLS_E2E=1`` and provide:
  * ``CORTRIX_BASE_URL`` / ``CORTRIX_API_KEY`` (live server)
  * ``ANTHROPIC_API_KEY`` (Claude / LangChain) and/or ``OPENAI_API_KEY`` (OpenAI)
"""

from __future__ import annotations

import os

import pytest

_E2E_ENABLED = os.environ.get("CORTRIX_SKILLS_E2E") == "1"

pytestmark = pytest.mark.skipif(
    not _E2E_ENABLED,
    reason="integration-deferred: real LLM round-trips run at the integration gate "
    "(set CORTRIX_SKILLS_E2E=1 + API keys to enable).",
)


def _live_kit():
    from cortrix_skills import CortrixToolKit

    return CortrixToolKit(
        base_url=os.environ["CORTRIX_BASE_URL"],
        api_key=os.environ.get("CORTRIX_API_KEY"),
    )


def test_langchain_react_invokes_all_29_methods():
    """LangChain ReAct agent reaches all 29 tools; errors raise ToolException."""
    pytest.skip("integration: implement against a live server + ChatAnthropic.")


def test_claude_tool_use_roundtrip():
    """Anthropic Messages tool_use loop succeeds; is_error=True passes 4 fields."""
    pytest.skip("integration: implement against a live server + Anthropic SDK.")


def test_openai_function_calling_roundtrip():
    """OpenAI function-calling loop succeeds (gpt-4o-mini); 4-field error JSON."""
    pytest.skip("integration: implement against a live server + OpenAI SDK.")
