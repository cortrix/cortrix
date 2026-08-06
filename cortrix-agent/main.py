"""Cortrix Agent — FastAPI entry point (agent V1.0 kernel).

Assembles the dependency graph and mounts the chat / sessions / config routers over the
UI-agnostic :mod:`agent_core` kernel:

    AsyncCortrix (Python SDK, dogfood)  ->  SdkRagProvider  ->  ChatExecutor
                                                              ^
                          LLM adapter (by config provider) ---+
    SessionStore (singleton, in-memory N=10)

Startup follows design section 11.bis: read config -> build the LLM provider ->
construct the Python SDK client (NOT connected in standalone) -> health-gate -> serve. In
standalone development cortrix-server is not running, so the section-4 health ping is
stubbed (``HEALTHCHECK_MODE=stub``, the default here) rather than failing fast with
``CX_ERR_AGENT_CORTRIX_SERVER_UNREACHABLE``.

Naming rule: this is the Cortrix Agent service — never a "chatbot".
"""

from __future__ import annotations

import logging
import os
import sys
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, Optional

import structlog
from fastapi import FastAPI

from config import settings

# The Python SDK is a sibling package (cortrix/sdk/python), not pip-installed in the
# standalone agent venv. Make it importable so agent dogfoods the SDK exactly like any
# external Agent (design section 5.2).
_SDK_PATH = Path(__file__).resolve().parents[1] / "sdk" / "python"
if _SDK_PATH.is_dir() and str(_SDK_PATH) not in sys.path:
    sys.path.insert(0, str(_SDK_PATH))

from agent_core import ChatExecutor, MemoryCoprocessor, SessionStore  # noqa: E402
from agent_core.sdk_rag import SdkRagProvider  # noqa: E402
from llm import (  # noqa: E402
    ClaudeAdapter,
    DeepSeekAdapter,
    GLMAdapter,
    MockAdapter,
    OllamaAdapter,
    OpenAIAdapter,
)
from llm.base import BaseLLMAdapter  # noqa: E402
from routes import chat as chat_routes  # noqa: E402
from routes import config as config_routes  # noqa: E402
from routes import sessions as session_routes  # noqa: E402

_LEVEL_MAP = {
    "DEBUG": logging.DEBUG,
    "INFO": logging.INFO,
    "WARNING": logging.WARNING,
    "ERROR": logging.ERROR,
}

structlog.configure(
    wrapper_class=structlog.make_filtering_bound_logger(
        _LEVEL_MAP.get(settings.log_level.upper(), logging.INFO)
    ),
)
logger = structlog.get_logger()


def _model_name() -> str:
    """Resolve the configured model name for the active provider (for meta.model_used)."""
    return {
        "openai": settings.openai_model,
        "claude": settings.claude_model,
        "ollama": settings.ollama_model,
        "glm": settings.glm_model,
        "deepseek": settings.deepseek_model,
        "mock": "mock-model",
    }.get(settings.llm_provider, "")


def _create_llm_adapter() -> BaseLLMAdapter:
    """Build the LLM adapter for the configured provider (design section 7.1, Step 3).

    6 providers are designed (MVP 5 + DeepSeek); the DeepSeek adapter is the V1.5
    ②-round scope, so an unknown/unbuilt provider safely falls back to Mock
    (retrieval-only) rather than failing startup.
    """
    provider = settings.llm_provider
    if provider == "openai":
        return OpenAIAdapter(
            api_key=settings.openai_api_key,
            model=settings.openai_model,
            base_url=settings.openai_base_url,
        )
    if provider == "claude":
        return ClaudeAdapter(
            api_key=settings.anthropic_api_key,
            model=settings.claude_model,
            max_tokens=settings.claude_max_tokens,
        )
    if provider == "ollama":
        return OllamaAdapter(
            base_url=settings.ollama_base_url,
            model=settings.ollama_model,
        )
    if provider == "glm":
        return GLMAdapter(
            api_key=settings.glm_api_key,
            model=settings.glm_model,
            base_url=settings.glm_base_url,
        )
    if provider == "deepseek":
        return DeepSeekAdapter(
            api_key=settings.deepseek_api_key,
            model=settings.deepseek_model,
            base_url=settings.deepseek_base_url,
        )
    return MockAdapter()


def build_app(
    *,
    rag_provider: Optional[SdkRagProvider] = None,
    llm_adapter: Optional[BaseLLMAdapter] = None,
    session_store: Optional[SessionStore] = None,
    model_name: Optional[str] = None,
    memory_coprocessor: Optional[MemoryCoprocessor] = None,
) -> FastAPI:
    """Construct the FastAPI app with the agent dependency graph wired.

    Components can be injected (tests pass mocks); otherwise they are built from
    ``config.settings``. The Python SDK client is constructed but NOT connected
    (standalone D3 discipline — design section 5 / sdk_rag TODO(D3.5)).
    """
    store = session_store or SessionStore()
    llm = llm_adapter if llm_adapter is not None else _create_llm_adapter()
    mdl = model_name if model_name is not None else _model_name()

    if rag_provider is not None:
        rag = rag_provider
    else:
        # Lazy import so the SDK path injection above is in effect.
        from cortrix import AsyncCortrix  # noqa: WPS433 (local import by design)

        client = AsyncCortrix(
            base_url=settings.cortrix_base_url,
            api_key=settings.cortrix_api_key or None,
        )
        rag = SdkRagProvider(
            client,
            namespace=settings.cortrix_namespace,
            top_k=settings.max_context_blocks,
        )

    executor = ChatExecutor(rag, llm, model_name=mdl)

    if memory_coprocessor is not None:
        mem = memory_coprocessor
    else:
        # Reuse the RAG provider's Python SDK client so memory co-processing dogfoods the same
        # SDK connection (design P-12). May be a stub client in standalone tests.
        mem_client = getattr(rag, "_client", None)
        mem = MemoryCoprocessor(mem_client, namespace=settings.cortrix_namespace)

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        # Design section 11.bis startup sequence (standalone: health ping stubbed).
        # HEALTHCHECK_MODE=live would attempt the cortrix-server ping with 5 retries
        # and raise CX_ERR_AGENT_CORTRIX_SERVER_UNREACHABLE on exhaustion (Step 4); the
        # live probe wiring is TODO(integration) once cortrix-server runs in deployment compose.
        healthcheck_mode = os.environ.get("HEALTHCHECK_MODE", "stub")
        logger.info(
            "cortrix_agent_started",
            llm_provider=settings.llm_provider,
            model=mdl,
            port=settings.agent_port,
            healthcheck_mode=healthcheck_mode,
        )
        yield
        close = getattr(getattr(rag, "_client", None), "close", None)
        if close is not None:
            try:
                await close()
            except Exception:  # noqa: BLE001 - best-effort shutdown
                pass
        logger.info("cortrix_agent_stopped")

    app = FastAPI(
        title="Cortrix Agent",
        description="Cortrix built-in Agent (V1.0 chat mode) over the SDK.",
        version="1.0.0-rc.1",
        lifespan=lifespan,
    )

    # --- DI: replace the route-layer NotImplementedError seams ---
    app.dependency_overrides[chat_routes.get_executor] = lambda: executor
    app.dependency_overrides[chat_routes.get_session_store] = lambda: store
    app.dependency_overrides[chat_routes.get_memory] = lambda: mem
    app.dependency_overrides[session_routes.get_session_store] = lambda: store
    app.dependency_overrides[config_routes.get_agent_llm_config] = _current_config_view
    app.dependency_overrides[config_routes.get_agent_llm_forwarder] = (
        lambda: _forward_agent_llm_config
    )

    app.include_router(chat_routes.router)
    app.include_router(session_routes.router)
    app.include_router(config_routes.router)

    # Agent-friendly validation errors (GEN-Agent #1): FastAPI's default 422 body
    # is a raw `{"detail": [...]}` array that an Agent cannot parse against the
    # CX_ERR envelope every other endpoint uses. Wrap it in the same 4-field shape
    # (code / category / retryable / structured_data) so a chat caller that sends a
    # malformed body gets a machine-readable, non-retryable validation error.
    from fastapi.encoders import jsonable_encoder  # local import (light module import)
    from fastapi.exceptions import RequestValidationError
    from fastapi.responses import JSONResponse
    from starlette.requests import Request as _Request

    @app.exception_handler(RequestValidationError)
    async def _validation_exception_handler(  # noqa: WPS430 (closure by FastAPI design)
        _request: _Request, exc: RequestValidationError
    ) -> JSONResponse:
        errors = exc.errors()
        first_msg = errors[0]["msg"] if errors else "request validation failed"
        return JSONResponse(
            status_code=422,
            content={
                "error": {
                    "code": "CX_ERR_AGENT_VALIDATION_ERROR",
                    "message": str(first_msg),
                    "category": "permanent",
                    "retryable": False,
                    "retry_after_ms": None,
                    "structured_data": {"validation_errors": jsonable_encoder(errors)},
                }
            },
        )

    @app.get("/health")
    async def health() -> dict[str, Any]:
        """Health check (design section 9.1).

        ``cortrix_server`` / ``llm_reachable`` are best-effort: in standalone the
        cortrix-server status is reported as ``unknown`` (no live ping — TODO(D3.5));
        LLM reachability uses the adapter's ``check_connection`` when available.
        """
        llm_reachable: Optional[bool] = None
        check = getattr(llm, "check_connection", None)
        if check is not None:
            try:
                llm_reachable = await check()
            except Exception:  # noqa: BLE001
                llm_reachable = False
        return {
            "status": "ready",
            "cortrix_server": "unknown",  # TODO(integration): live ping in deployment compose
            "llm_reachable": llm_reachable,
        }

    return app


async def _forward_agent_llm_config(payload: dict[str, Any]) -> dict[str, Any]:
    """Forward an agent_llm update to cortrix-server (design section 6.3 / P4).

    PUTs to ``{cortrix_base_url}/api/v1/system/agent_llm_config`` — the authoritative
    C++ IGlobalConfig admin endpoint — and returns its JSON body (the masked,
    persisted config). Raises on a transport error or a non-2xx response so the route
    surfaces a 502.
    """
    import httpx  # local import: keeps the module import light + mirrors the SDK lazy-load

    url = settings.cortrix_base_url.rstrip("/") + "/api/v1/system/agent_llm_config"
    async with httpx.AsyncClient(timeout=10.0) as client:
        resp = await client.put(url, json=payload)
        resp.raise_for_status()
        return resp.json()


def _current_config_view() -> config_routes.AgentLlmConfigView:
    """Build the display config (api_key masked) from settings for GET /config."""
    provider = settings.llm_provider
    api_key = {
        "openai": settings.openai_api_key,
        "claude": settings.anthropic_api_key,
        "glm": settings.glm_api_key,
    }.get(provider, "")
    base_url = {
        "openai": settings.openai_base_url,
        "ollama": settings.ollama_base_url,
        "glm": settings.glm_base_url,
    }.get(provider, "")
    return config_routes.AgentLlmConfigView(
        llm_provider=provider,
        model=_model_name(),
        base_url=base_url,
        api_key_masked=config_routes._mask_api_key(api_key),
        max_tokens=settings.claude_max_tokens,
        temperature=0.7,
    )


app = build_app()
