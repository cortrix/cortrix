"""POST /chat — Cortrix Agent streaming chat endpoint (design section 9.1).

This route is a thin SSE encoder over :class:`agent_core.ChatExecutor`. The executor
owns the RAG -> prompt -> LLM pipeline and the L1/L2/L3 degradation policy (design
section 9.2); the route's only jobs are:

  1. Parse the multi-tenant headers (Authorization / X-Cortrix-Tenant-Id /
     X-Cortrix-Namespace) and the ``?explain`` / ``?debug`` query flags.
  2. Assemble a ``ChatContext`` (pulling prior turns from the SessionStore — P-2 N=10).
  3. Encode each executor ``StreamEvent`` as an SSE frame per design section 9.1:
     ``data:{chunk}`` (many) -> ``data:{meta}`` -> ``data:[DONE]``.
  4. Persist the completed turn back to the SessionStore (from ``meta["_assistant_text"]``).
  5. Map any :class:`AgentError` to the GEN-Agent 4-field error SSE event (section 9.3).

Naming rule: this is the Cortrix Agent chat endpoint — never a
"chatbot" endpoint; the executor class is ``ChatExecutor``.

MEM co-processing (interaction_log write / memory extract trigger / memory isolation user_id
filter — design the agent design) is the V1.5 ②-round scope and is intentionally NOT
wired here; see the ``TODO(D3.5)`` marker below.
"""

from __future__ import annotations

import asyncio
import json
from typing import Optional

import structlog
from fastapi import APIRouter, Depends, Header, Request
from pydantic import BaseModel, Field
from sse_starlette.sse import EventSourceResponse

from agent_core import AgentError, ChatContext, ChatExecutor, MemoryCoprocessor, SessionStore

logger = structlog.get_logger()

router = APIRouter()


class ChatRequest(BaseModel):
    """POST /chat request body (design section 9.1).

    ``user_id`` is carried for the V1.5 memory isolation user-isolation filter (design
    the agent design1); in V1.0 it is accepted but only threaded into the context.
    ``demo_flow`` from the MVP agent-demo is intentionally dropped (V1.5 removal,
    section 9.1) — the V1.0 ChatExecutor runs a single fixed RAG flow.
    """

    message: str = Field(..., min_length=1, max_length=32768)
    session_id: str = Field(..., min_length=1, max_length=128)
    user_id: Optional[str] = None


# --- Dependency seams (overridden in main.py via app.dependency_overrides) ---


def get_executor() -> ChatExecutor:
    """Injected from main.py (DI wiring)."""
    raise NotImplementedError("ChatExecutor dependency not wired (see main.py).")


def get_session_store() -> SessionStore:
    """Injected from main.py (DI wiring)."""
    raise NotImplementedError("SessionStore dependency not wired (see main.py).")


def get_memory() -> MemoryCoprocessor:
    """Injected from main.py (DI wiring)."""
    raise NotImplementedError("MemoryCoprocessor dependency not wired (see main.py).")


# Fire-and-forget memory co-processing tasks, kept referenced so the event loop does not
# garbage-collect them mid-flight (design the agent design — never blocks the chat response).
_BACKGROUND_TASKS: set = set()


def _truthy(value: Optional[str]) -> bool:
    """Parse a query flag (``?explain`` / ``?explain=true``) into a bool."""
    if value is None:
        return False
    return value.strip().lower() in ("", "1", "true", "yes", "on")


@router.post("/chat")
async def chat_endpoint(
    request: Request,
    body: ChatRequest,
    authorization: Optional[str] = Header(default=None),
    x_cortrix_tenant_id: Optional[str] = Header(default=None),
    x_cortrix_namespace: Optional[str] = Header(default=None),
    executor: ChatExecutor = Depends(get_executor),
    sessions: SessionStore = Depends(get_session_store),
    mem: MemoryCoprocessor = Depends(get_memory),
):
    """Stream a Cortrix Agent chat turn as SSE (design section 9.1).

    Query params: ``?explain=true`` exposes B-class meta; ``?debug=true`` exposes
    C-class ``*_call_failed_detail`` on failure (design section 1.7).
    """
    explain = _truthy(request.query_params.get("explain"))
    debug = _truthy(request.query_params.get("debug"))

    # Bearer token is parsed for forward-compat (P-5); V1.0 CE does not validate it
    # and tenant_id comes from the header. In V1.5 Cloud the JWT claim takes priority
    # (design section 8.2 anti-BOLA) — TODO(D3.5) wire JWT claim extraction.
    _ = authorization  # reserved (agent design)

    # A-class tenant_id: None in CE single-tenant; echoes the header when provided
    # (design section 1.7 / 8.2 P-5 forward-compat passthrough).
    tenant_id = x_cortrix_tenant_id or None

    # Pull prior turns (P-2 N=10 sliding window) so the executor can build multi-turn
    # context; ensures the session row exists for the write-back below.
    sessions.get_or_create(body.session_id, tenant_id=tenant_id)
    history = sessions.recent_messages(body.session_id)

    context = ChatContext(
        session_id=body.session_id,
        tenant_id=tenant_id,
        namespace=x_cortrix_namespace or None,
        user_id=body.user_id,
        explain=explain,
        debug=debug,
        history=history,
    )

    async def event_stream():
        assistant_text = ""
        try:
            async for event in executor.execute(body.message, context):
                if event.kind == "chunk":
                    yield {"data": json.dumps({"chunk": event.chunk})}
                elif event.kind == "meta":
                    meta = dict(event.meta or {})
                    # _assistant_text is an internal channel for session write-back;
                    # strip it from the wire meta (design section 9.1 meta shape).
                    assistant_text = meta.pop("_assistant_text", "")
                    yield {"data": json.dumps({"meta": meta})}
            yield {"data": "[DONE]"}

            # Persist the completed turn (user + assistant) to the in-memory store.
            sessions.append_turn(
                body.session_id,
                body.message,
                assistant_text,
                tenant_id=tenant_id,
            )
            # Feed the turn back into Cortrix memory (agent trace interaction_log + memory extract,
            # via one SDK memory.log call) fire-and-forget so it never blocks the response
            # (design the agent design). TODO(D3.5): real cortrix-server interaction-log
            # persistence + live memory extraction worker; memory isolation user_id from a real JWT claim.
            task = asyncio.create_task(
                mem.record_turn(
                    body.session_id,
                    body.message,
                    assistant_text,
                    user_id=body.user_id,
                    namespace=context.namespace,
                )
            )
            _BACKGROUND_TASKS.add(task)
            task.add_done_callback(_BACKGROUND_TASKS.discard)
        except AgentError as err:
            # GEN-Agent 4-field error envelope (design section 9.3).
            logger.warning("chat_agent_error", code=err.code, session_id=body.session_id)
            yield {"event": "error", "data": json.dumps(err.to_dict())}
            yield {"data": "[DONE]"}
        except Exception as exc:  # noqa: BLE001 - last-resort guard -> generic 4-field error
            logger.error("chat_unhandled_error", error=str(exc), session_id=body.session_id)
            fallback = AgentError(
                "CX_ERR_F48_RAG_FAILED",
                message="Unhandled error during chat generation",
                structured_data={"detail": str(exc)},
            )
            yield {"event": "error", "data": json.dumps(fallback.to_dict())}
            yield {"data": "[DONE]"}

    return EventSourceResponse(event_stream())
