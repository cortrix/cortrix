"""Extra coverage for optional branches: header assembly, non-JSON error bodies,
optional tool params, and the main() entry point."""

from __future__ import annotations

from unittest.mock import MagicMock, patch

import httpx
import pytest
from conftest import get_tool_fn, make_response

from cortrix_mcp import transport
from mcp import McpError


def test_headers_without_api_key(monkeypatch):
    monkeypatch.setattr(transport, "CORTRIX_API_KEY", "")
    h = transport._headers()
    assert "Authorization" not in h
    assert h["Content-Type"] == "application/json"


def test_headers_with_api_key_and_extra(monkeypatch):
    monkeypatch.setattr(transport, "CORTRIX_API_KEY", "sk-test")
    h = transport._headers({"X-Custom": "1"})
    assert h["Authorization"] == "Bearer sk-test"
    assert h["X-Custom"] == "1"


def test_non_json_error_body_falls_back_to_text(mock_request):
    resp = make_response(status_code=500, json_body=None)
    resp.json.side_effect = ValueError("not json")
    resp.text = "upstream boom"
    exc = httpx.HTTPStatusError("500", request=MagicMock(), response=resp)
    mock_request.set_side_effect(exc)
    with pytest.raises(McpError) as ei:
        get_tool_fn("cortrix_health")()
    # Falls back to CX_ERR_INTERNAL_ERROR with the text surfaced as message.
    assert ei.value.error.data["code"] == "CX_ERR_INTERNAL_ERROR"
    assert ei.value.error.message == "upstream boom"


def test_upload_includes_metadata_branch(mock_request):
    mock_request.set(json_body={"task_id": "t"})
    get_tool_fn("cortrix_upload")(content="x", metadata={"k": "v"})
    assert mock_request.last_call.kwargs["json"]["metadata"] == {"k": "v"}


def test_async_upload_includes_filename_and_metadata(mock_request):
    mock_request.set(json_body={"task_id": "t"})
    get_tool_fn("cortrix_async_upload")(content="x", filename="big.pdf", metadata={"k": "v"})
    body = mock_request.last_call.kwargs["json"]
    assert body["filename"] == "big.pdf"
    assert body["metadata"] == {"k": "v"}


def test_memory_get_audit_with_memory_id_filter(mock_request):
    mock_request.set(json_body={"entries": []})
    get_tool_fn("cortrix_memory_get_audit")(memory_id="m1")
    assert mock_request.last_call.kwargs["params"]["memory_id"] == "m1"


def test_list_operations_passes_all_filters(mock_request):
    mock_request.set(json_body={"operations": []})
    get_tool_fn("cortrix_list_operations")(
        user_id="u1", namespace="ns", action="memory_create",
        start_time="2026-01-01T00:00:00Z", end_time="2026-02-01T00:00:00Z",
        limit=500, cursor="abc",
    )
    params = mock_request.last_call.kwargs["params"]
    assert params["user_id"] == "u1"
    assert params["action"] == "memory_create"
    assert params["cursor"] == "abc"
    assert params["limit"] == 200  # clamped to max 200


def test_memory_invalidate_without_optional_params(mock_request):
    mock_request.set(json_body={"memory_id": "m", "status": "invalidated"})
    get_tool_fn("cortrix_memory_invalidate")(memory_id="m1")
    # No namespace/reason -> params is None.
    assert mock_request.last_call.kwargs["params"] is None


def test_main_invokes_mcp_run():
    with patch("cortrix_mcp.server.mcp.run") as run:
        from cortrix_mcp.server import main

        main()
        run.assert_called_once()
