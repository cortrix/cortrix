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


def test_memory_get_audit_maps_to_operations(mock_request):
    # No dedicated /memory/audit route: maps to GET /operations filtered to the memory
    # lifecycle actions. memory_id is NOT a backend param (operation_log has no such
    # filter) — it is applied client-side over the returned rows' resource_id.
    mock_request.set(json_body={"operations": [
        {"op_id": "o1", "resource_id": "m1", "action": "memory_invalidate"},
        {"op_id": "o2", "resource_id": "m2", "action": "memory_extract"},
    ]})
    out = get_tool_fn("cortrix_memory_get_audit")(memory_id="m1")
    args, kwargs = mock_request.last_call
    assert args[0] == "GET"
    assert args[1].endswith("/api/v1/operations")
    assert kwargs["params"]["action_in"] == "memory_extract,memory_invalidate,memory_revoke"
    assert "memory_id" not in kwargs["params"]
    # Client-side resource_id filter keeps only the requested memory's row.
    assert out["data"] == [{"op_id": "o1", "resource_id": "m1", "action": "memory_invalidate"}]


def test_memory_get_audit_without_memory_id_returns_all(mock_request):
    mock_request.set(json_body={"operations": [{"op_id": "o1", "resource_id": "m1"}]})
    out = get_tool_fn("cortrix_memory_get_audit")()
    assert out["data"] == [{"op_id": "o1", "resource_id": "m1"}]


def test_list_operations_passes_all_filters(mock_request):
    # Param names mirror the backend operations route (operations_routes.cpp):
    # namespace_id / from_timestamp / to_timestamp (Unix ms) / offset / action_in.
    mock_request.set(json_body={"operations": []})
    get_tool_fn("cortrix_list_operations")(
        user_id="u1", namespace="ns", action="memory_create",
        action_in=["memory_invalidate", "memory_revoke"],
        start_time=1735689600000, end_time=1738368000000,
        limit=500, offset=20, sort_order="ASC",
    )
    params = mock_request.last_call.kwargs["params"]
    assert params["user_id"] == "u1"
    assert params["namespace_id"] == "ns"
    assert params["action"] == "memory_create"
    assert params["action_in"] == "memory_invalidate,memory_revoke"
    assert params["from_timestamp"] == 1735689600000
    assert params["to_timestamp"] == 1738368000000
    assert params["offset"] == 20
    assert params["sort_order"] == "ASC"
    assert params["limit"] == 200  # clamped to max 200


def test_list_interactions_flattens_filter_to_backend_params(mock_request):
    # GET /interactions (observability_routes.cpp) reads flat params: namespace_id +
    # user_id/session_id/from_timestamp/to_timestamp/sort_order. The MCP filter sub-object
    # is flattened (from_ts -> from_timestamp, to_ts -> to_timestamp); namespace -> namespace_id.
    mock_request.set(json_body={"interactions": []})
    get_tool_fn("cortrix_list_interactions")(
        namespace="default",
        user_id="u1",
        filter={"session_id": "s1", "from_ts": 1, "to_ts": 2, "sort_order": "ASC", "bogus": "x"},
    )
    args, kwargs = mock_request.last_call
    assert args[0] == "GET"
    assert args[1].endswith("/api/v1/interactions")
    params = kwargs["params"]
    assert params["namespace_id"] == "default"
    assert "namespace" not in params  # renamed, not duplicated
    assert "filter" not in params  # flattened, not nested
    assert params["session_id"] == "s1"
    assert params["from_timestamp"] == 1
    assert params["to_timestamp"] == 2
    assert params["sort_order"] == "ASC"
    assert "bogus" not in params  # out-of-whitelist dropped


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
