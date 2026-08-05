"""GET /config + agent_llm admin config (design section 6.3 / 9.1).

V1.0 surface:
  * ``GET /config`` — current agent LLM config with the ``api_key`` masked
    (admin-facing; design section 9.1). Never returns a raw key.
  * ``GET /config/providers`` — the provider/model catalog the web UI
    ``LLMSettingsDialog`` reuses (design section 6.4), with a ``configured`` flag.
  * ``PUT /config/agent_llm`` — update the agent LLM config (admin only).

In V1.0 the authoritative agent_llm config lives in the C++ ``IGlobalConfig`` and is
mutated through the cortrix-server admin endpoint ``PUT /api/v1/system/agent_llm_config``
(design section 6.2 / 6.3). ``PUT /config/agent_llm`` here forwards to that endpoint
(D3.5 r2 · Wave P · P4) so the Settings UI has a single same-origin write path; the
Python side does not own a durable config store.

Demo-only routes from the MVP agent-demo (``/demo-flows``, the ``.env``-writing role
machinery, the cortrix_client-backed ``GET /config`` health probe) are intentionally
removed.
"""

from __future__ import annotations

from typing import Any, Awaitable, Callable, Optional

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel

router = APIRouter()


# Provider catalog reused by the web UI LLMSettingsDialog (design section 6.4 / 7.1).
# 6 providers = MVP 5 + DeepSeek (issue 5 decision B). DeepSeek adapter itself is the
# V1.5 ②-round scope (not added in this ①-kernel round) but the catalog lists it so the
# Settings UI can render the option.
_PROVIDERS: list[dict[str, Any]] = [
    {"id": "openai", "name": "OpenAI", "needs_key": True, "needs_url": False},
    {"id": "claude", "name": "Anthropic Claude", "needs_key": True, "needs_url": False},
    {"id": "ollama", "name": "Ollama (Local)", "needs_key": False, "needs_url": True},
    {"id": "glm", "name": "Zhipu GLM", "needs_key": True, "needs_url": False},
    {"id": "mock", "name": "Retrieval Only (Mock)", "needs_key": False, "needs_url": False},
    {"id": "deepseek", "name": "DeepSeek", "needs_key": True, "needs_url": False},
]


def _mask_api_key(api_key: Optional[str]) -> Optional[str]:
    """Mask a secret for display (design section 9.1 — GET /config desensitizes api_key)."""
    if not api_key:
        return None
    if len(api_key) <= 8:
        return "****"
    return api_key[:4] + "****" + api_key[-4:]


class AgentLlmConfigView(BaseModel):
    """Read model exposed by ``GET /config`` (api_key masked; design section 9.1)."""

    llm_provider: str
    model: str
    base_url: str = ""
    api_key_masked: Optional[str] = None
    max_tokens: int = 4096
    temperature: float = 0.7


def get_agent_llm_config() -> AgentLlmConfigView:
    """Injected from main.py — the current (display) agent LLM config.

    Sourced from the local Settings in V1.0; becomes the C++ IGlobalConfig
    ``GetAgentLlmConfig`` mirror in D3.5.
    """
    raise NotImplementedError("agent_llm config dependency not wired (see main.py).")


class AgentLlmConfigUpdate(BaseModel):
    """PUT /config/agent_llm body (admin only)."""

    provider: str
    model: Optional[str] = None
    api_key: Optional[str] = None
    base_url: Optional[str] = None
    max_tokens: Optional[int] = None
    temperature: Optional[float] = None


# Injected from main.py — forwards an agent_llm update to the cortrix-server admin
# endpoint and returns its JSON body. Raising on transport / non-2xx failures.
AgentLlmForwarder = Callable[[dict[str, Any]], Awaitable[dict[str, Any]]]


def get_agent_llm_forwarder() -> AgentLlmForwarder:
    """Injected from main.py — the cortrix-server PUT forwarder (see main.py)."""
    raise NotImplementedError("agent_llm forwarder not wired (see main.py).")


@router.get("/config", response_model=AgentLlmConfigView)
async def get_config(config: AgentLlmConfigView = Depends(get_agent_llm_config)):
    """Return the current agent LLM config with the API key masked (admin-facing)."""
    return config


@router.get("/config/providers")
async def get_providers():
    """Return the provider catalog reused by the web UI LLMSettingsDialog (no secrets)."""
    return {"providers": _PROVIDERS}


@router.put("/config/agent_llm")
async def update_agent_llm(
    body: AgentLlmConfigUpdate,
    forward: AgentLlmForwarder = Depends(get_agent_llm_forwarder),
):
    """Update the agent LLM config (admin only).

    Forwards to the cortrix-server admin endpoint ``PUT
    /api/v1/system/agent_llm_config`` (design section 6.2/6.3), which owns the
    authoritative C++ ``IGlobalConfig.SetAgentLlmConfig`` (api_key encrypted at
    rest). Returns the server's masked, persisted config. A field left unset is
    omitted from the forwarded body (server-side merge semantics).
    """
    payload = body.model_dump(exclude_none=True)
    try:
        return await forward(payload)
    except Exception as exc:  # transport / non-2xx → 502 (the server is upstream)
        raise HTTPException(
            status_code=502,
            detail=f"failed to forward agent_llm config to cortrix-server: {exc}",
        ) from exc
