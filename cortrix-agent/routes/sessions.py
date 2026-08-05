"""GET /sessions/:session_id — Cortrix Agent session history (design section 9.1).

Session state is the in-process :class:`agent_core.SessionStore` (decision P-2): an
in-memory dict with an N=10 sliding multi-turn window. There is NO server round-trip
in V1.0 — memory extraction durable-session integration is the V1.5 upgrade (TODO(integration)).

The response shape (design section 9.1)::

    {session_id, messages:[{role, content, timestamp}], window_size, created_at, tenant_id}

A missing session returns the GEN-Agent 4-field error ``CX_ERR_AGENT_SESSION_NOT_FOUND``
(design section 9.3 / 10.1), HTTP 404.
"""

from __future__ import annotations

from typing import Optional

from fastapi import APIRouter, Depends, Header
from fastapi.responses import JSONResponse

from agent_core import AgentError, SessionStore

router = APIRouter()


def get_session_store() -> SessionStore:
    """Injected from main.py (DI wiring)."""
    raise NotImplementedError("SessionStore dependency not wired (see main.py).")


@router.get("/sessions/{session_id}")
async def get_session_detail(
    session_id: str,
    authorization: Optional[str] = Header(default=None),
    x_cortrix_tenant_id: Optional[str] = Header(default=None),
    sessions: SessionStore = Depends(get_session_store),
):
    """Return a session's windowed history (design section 9.1).

    ``Authorization`` / ``X-Cortrix-Tenant-Id`` are accepted for forward-compat (P-5);
    V1.0 CE does not validate them.
    """
    _ = (authorization, x_cortrix_tenant_id)  # reserved (P-5 forward-compat)

    payload = sessions.get_history(session_id)
    if payload is None:
        err = AgentError(
            "CX_ERR_AGENT_SESSION_NOT_FOUND",
            structured_data={"session_id": session_id},
        )
        return JSONResponse(status_code=err.http_status, content=err.to_dict())
    return payload
