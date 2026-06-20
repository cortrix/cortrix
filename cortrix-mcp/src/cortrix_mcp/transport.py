"""HTTP transport to cortrix-server + shared request/response plumbing.

Continues the MVP design: a single httpx client with ``proxy=None`` so local
loopback connections are never routed through a system proxy. Every tool calls
through ``request()`` here, which:
  * prefixes the path with ``{CORTRIX_URL}/api/v1`` (MCP talks HTTP directly, not via
    the Python SDK, so it carries its own ``/api/v1`` — brief section 3),
  * injects the Bearer token when ``CORTRIX_API_KEY`` is set,
  * maps timeout / connection-refused to the CX_ERR_MCP_* transient codes,
  * passes business 4xx/5xx errors through unchanged (errors.passthrough_business_error),
  * builds the GEN-Agent success envelope with structured_data (trace_id, session_id, ...).
"""

from __future__ import annotations

import os
import uuid
from typing import Any, Optional

import httpx

from .errors import mcp_error, passthrough_business_error, success_envelope

# ---------------------------------------------------------------------------
# Configuration (feature design section 5.3).
# NOTE: default port is 8420 (config.h int port = 8420, post WC-R4 port reconcile;
# the historical 8080/9090 ports are stale and intentionally NOT propagated here).
# ---------------------------------------------------------------------------
CORTRIX_URL = os.environ.get("CORTRIX_URL", "http://127.0.0.1:8420")
CORTRIX_NAMESPACE = os.environ.get("CORTRIX_NAMESPACE", "default")
CORTRIX_API_KEY = os.environ.get("CORTRIX_API_KEY", "")
CORTRIX_MCP_ADMIN = os.environ.get("CORTRIX_MCP_ADMIN", "false").lower() == "true"
try:
    CORTRIX_MCP_TIMEOUT = float(os.environ.get("CORTRIX_MCP_TIMEOUT", "30"))
except ValueError:
    CORTRIX_MCP_TIMEOUT = 30.0

API_PREFIX = "/api/v1"

# Single shared client — bypass system proxy for local connections (MVP behaviour).
_transport = httpx.HTTPTransport(proxy=None)
_client = httpx.Client(transport=_transport, timeout=CORTRIX_MCP_TIMEOUT)

# Process-scoped MCP session id. D3.5 deferred: real per-session capture wires to
# ARCH section 2.12 agent_trace once the live cortrix-server connection exists.
_SESSION_ID = "mcp-session-" + uuid.uuid4().hex[:12]


def mcp_session_id() -> str:
    """Return the current MCP session id (ARCH section 2.12 auto-capture).

    D3.5 deferred: standalone uses a process-scoped id; live session binding to the
    cortrix-server agent_trace happens during integration.
    """
    return _SESSION_ID


def _headers(extra: Optional[dict[str, str]] = None) -> dict[str, str]:
    h = {"Content-Type": "application/json"}
    if CORTRIX_API_KEY:
        h["Authorization"] = f"Bearer {CORTRIX_API_KEY}"
    if extra:
        h.update(extra)
    return h


def _structured_data(resp: httpx.Response, business_meta: dict[str, Any]) -> dict[str, Any]:
    """Assemble structured_data for a success envelope (feature design section 6.1)."""
    sd: dict[str, Any] = {
        "trace_id": resp.headers.get("X-Trace-Id"),
        "session_id": mcp_session_id(),
    }
    # Surface Class-A data-integrity fields when the backend reports them.
    for key in ("coverage_ratio", "namespaces_failed", "total", "has_more", "next_cursor"):
        if key in business_meta:
            sd[key] = business_meta[key]
    return sd


def request(
    method: str,
    path: str,
    *,
    json_body: Optional[dict[str, Any]] = None,
    params: Optional[dict[str, Any]] = None,
    headers: Optional[dict[str, str]] = None,
    timeout: Optional[float] = None,
    data_key: Optional[str] = None,
) -> dict[str, Any]:
    """Call cortrix-server and return a GEN-Agent success envelope.

    Args:
        method: HTTP verb (GET / POST / PATCH / DELETE / PUT).
        path: API path WITHOUT the /api/v1 prefix (e.g. "/query").
        json_body: JSON request body.
        params: query-string parameters.
        headers: extra headers (merged on top of Content-Type + Bearer).
        timeout: per-call timeout override (seconds).
        data_key: if given, the envelope ``data`` is ``resp_json[data_key]``; otherwise
            the whole decoded JSON body is used as ``data``.

    Raises:
        McpError: timeout/unavailable map to CX_ERR_MCP_* transient codes; HTTP 4xx/5xx
            pass through the backend business error unchanged.
    """
    url = f"{CORTRIX_URL}{API_PREFIX}{path}"
    try:
        resp = _client.request(
            method,
            url,
            json=json_body,
            params=params,
            headers=_headers(headers),
            timeout=timeout if timeout is not None else CORTRIX_MCP_TIMEOUT,
        )
        resp.raise_for_status()
    except httpx.TimeoutException as exc:
        raise mcp_error(
            "CX_ERR_MCP_BACKEND_TIMEOUT",
            structured_data={
                "original_url": url,
                "timeout_seconds": timeout if timeout is not None else CORTRIX_MCP_TIMEOUT,
                "detail": str(exc),
            },
        ) from exc
    except httpx.ConnectError as exc:
        raise mcp_error(
            "CX_ERR_MCP_BACKEND_UNAVAILABLE",
            structured_data={"original_url": url, "detail": str(exc)},
        ) from exc
    except httpx.HTTPStatusError as exc:
        try:
            error_body = exc.response.json().get("error", {})
        except Exception:  # noqa: BLE001 - non-JSON error body
            error_body = {"message": exc.response.text}
        raise passthrough_business_error(exc.response.status_code, error_body) from exc

    # 204 No Content (e.g. delete watcher) — no JSON body.
    if resp.status_code == 204 or not resp.content:
        return success_envelope({}, _structured_data(resp, {}))

    payload = resp.json()
    business_meta = payload.get("meta", {}) if isinstance(payload, dict) else {}
    data = payload.get(data_key) if (data_key and isinstance(payload, dict)) else payload
    return success_envelope(data, _structured_data(resp, business_meta))


def require_admin() -> None:
    """Enforce the admin scope for the 2 F16a admin tools (feature design section 4.6).

    V1.0 uses the ``CORTRIX_MCP_ADMIN=true`` env fallback (risk #4); after P08 D3 the
    check switches back to the Bearer ``role=admin`` claim. D3.5 deferred: live Bearer
    claim parsing wires to P08 once the real auth path is available.
    """
    if not CORTRIX_MCP_ADMIN:
        raise mcp_error(
            "CX_ERR_MCP_ADMIN_REQUIRED",
            structured_data={"hint": "set CORTRIX_MCP_ADMIN=true or use a role=admin Bearer token"},
        )
