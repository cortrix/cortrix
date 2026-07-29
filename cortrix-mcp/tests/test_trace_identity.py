"""Issue #25 — the transport must propagate session/trace identity to the backend.

cortrix-server validates X-Session-Id / X-Trace-Id / X-Agent-Id against the identity
whitelist ([a-zA-Z0-9_.:/-], <=128 chars) and adopts valid values verbatim
(http_observability_middleware.cpp), so the envelope must report exactly the identity
the server-side agent_trace recorded. These tests pin the acceptance criteria from
the issue: stable session id, unique per-call trace id, stable agent id, identity in
success AND error envelopes, and no secrets leaking into any envelope.
"""

from __future__ import annotations

import json
import re
from unittest.mock import MagicMock

import httpx
from conftest import call_tool_result, get_tool_fn, make_response

from cortrix_mcp import transport

# Mirror of the server-side whitelist (observability_context.cpp IsValidFormat).
SERVER_IDENTITY_RE = re.compile(r"[A-Za-z0-9_.:/-]{1,128}")


def _sent_headers(mock_request) -> dict:
    return mock_request.last_call.kwargs["headers"]


def test_identity_headers_sent_stable_session_unique_trace(mock_request):
    """Two consecutive calls: same X-Session-Id, different X-Trace-Id (criteria 1/2/6)."""
    mock_request.set(json_body={"status": "ok"})
    get_tool_fn("cortrix_health")()
    first = dict(_sent_headers(mock_request))
    get_tool_fn("cortrix_health")()
    second = dict(_sent_headers(mock_request))

    for h in (first, second):
        for name in ("X-Session-Id", "X-Trace-Id", "X-Agent-Id"):
            assert name in h
            assert SERVER_IDENTITY_RE.fullmatch(h[name]), f"{name} fails server whitelist"

    assert first["X-Session-Id"] == second["X-Session-Id"] == transport.mcp_session_id()
    assert first["X-Agent-Id"] == second["X-Agent-Id"] == transport.CORTRIX_AGENT_ID
    assert first["X-Trace-Id"] != second["X-Trace-Id"]


def test_envelope_reports_sent_trace_id_when_server_does_not_echo(mock_request):
    """No X-Trace-Id on the response (today's server): envelope trace_id == sent id.

    NOTE: conftest.make_response treats a falsy ``headers`` as "use the default echo
    headers", so pass a truthy dict WITHOUT X-Trace-Id to model today's server.
    """
    mock_request.set(json_body={"status": "ok"}, headers={"Content-Type": "application/json"})
    env = get_tool_fn("cortrix_health")()
    sent = _sent_headers(mock_request)["X-Trace-Id"]
    assert env["meta"]["structured_data"]["trace_id"] == sent
    assert env["meta"]["structured_data"]["session_id"] == transport.mcp_session_id()


def test_envelope_prefers_server_echoed_trace_id(mock_request):
    """If a future server echoes X-Trace-Id, the echo is authoritative."""
    mock_request.set(json_body={"status": "ok"}, headers={"X-Trace-Id": "server-echo-1"})
    env = get_tool_fn("cortrix_health")()
    assert env["meta"]["structured_data"]["trace_id"] == "server-echo-1"


def test_transient_error_envelope_carries_identity(mock_request):
    """Timeout/unavailable envelopes keep session + trace identity (criterion 5)."""
    mock_request.set_side_effect(httpx.TimeoutException("slow"))
    result = call_tool_result("cortrix_health")
    assert result.is_error is True
    sd = result.structured_content["structured_data"]
    assert sd["session_id"] == transport.mcp_session_id()
    assert SERVER_IDENTITY_RE.fullmatch(sd["trace_id"])


def test_business_error_identity_merged_backend_keys_win(mock_request):
    """Passthrough gains caller identity, but backend structured_data is never altered."""
    err_resp = make_response(
        status_code=404,
        json_body={"error": {"code": "CX_ERR_F13_SESSION_NOT_FOUND", "retryable": False,
                              "category": "permanent",
                              "structured_data": {"session_id": "queried-target-sid"}}},
    )
    exc = httpx.HTTPStatusError("404", request=MagicMock(), response=err_resp)
    mock_request.set_side_effect(exc)
    result = call_tool_result("cortrix_health")
    assert result.is_error is True
    data = result.structured_content
    assert data["code"] == "CX_ERR_F13_SESSION_NOT_FOUND"  # passthrough unchanged
    sd = data["structured_data"]
    # Backend's own session_id field (the *queried* session) wins over caller identity.
    assert sd["session_id"] == "queried-target-sid"
    # Caller trace id is still present for correlation.
    assert SERVER_IDENTITY_RE.fullmatch(sd["trace_id"])


def test_no_secret_in_success_or_error_envelopes(mock_request, monkeypatch):
    """Authorization material must never enter envelopes (criterion 8)."""
    secret = "sk-test-secret-123"
    monkeypatch.setattr(transport, "CORTRIX_API_KEY", secret)

    mock_request.set(json_body={"status": "ok"})
    env = get_tool_fn("cortrix_health")()
    assert secret not in json.dumps(env)
    # The wire request itself carries the Bearer token — that is the auth path.
    assert _sent_headers(mock_request)["Authorization"] == f"Bearer {secret}"

    mock_request.set_side_effect(httpx.TimeoutException("slow"))
    result = call_tool_result("cortrix_health")
    assert result.is_error is True
    assert secret not in json.dumps(result.model_dump(by_alias=True))


def test_resolve_agent_id_accepts_valid_and_falls_back_on_invalid():
    """CORTRIX_AGENT_ID override: valid values pass, invalid ones fall back."""
    assert transport._resolve_agent_id("my-agent_01.x:/y") == "my-agent_01.x:/y"
    assert transport._resolve_agent_id("bad value!") == "cortrix-mcp"
    assert transport._resolve_agent_id("") == "cortrix-mcp"
    assert transport._resolve_agent_id("x" * 200) == "cortrix-mcp"
    assert transport._resolve_agent_id("\u4e2d\u6587id") == "cortrix-mcp"
