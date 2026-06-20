"""Unit tests with mocked HTTP for the 29 main tools + errors/transport plumbing.

Every successful tool call asserts the full GEN-Agent 4-field envelope is present
(meta.retryable / meta.category / meta.retry_after_ms / meta.structured_data). Error
paths cover CX_ERR_MCP_* (timeout / unavailable) and business pass-through (4xx).
"""

from __future__ import annotations

from unittest.mock import MagicMock

import httpx
import pytest
from conftest import get_tool_fn, make_response

from cortrix_mcp import errors, transport
from mcp import McpError


# ---------------------------------------------------------------------------
# Envelope / 4-field assertions shared helper.
# ---------------------------------------------------------------------------
def assert_envelope(out, expect_category="success"):
    assert set(out.keys()) == {"data", "meta"}
    meta = out["meta"]
    assert set(meta.keys()) == {"retryable", "category", "retry_after_ms", "structured_data"}
    assert meta["category"] == expect_category
    assert isinstance(meta["structured_data"], dict)
    # session_id is auto-captured (ARCH 2.12); trace_id surfaced from response header.
    assert "session_id" in meta["structured_data"]
    return out["data"]


# ---------------------------------------------------------------------------
# Table-driven success coverage: every one of the 29 main tools, >=1 success call.
# (name, kwargs, json_body returned by the mocked backend, optional asserts on data)
# ---------------------------------------------------------------------------
SUCCESS_CASES = [
    # --- core 12 ---
    ("cortrix_health", {}, {"status": "ok", "version": "1.0.0"}),
    ("cortrix_query", {"query": "hello"}, {"data": [{"score": 0.9}], "meta": {"coverage_ratio": 1.0}}),
    ("cortrix_upload", {"content": "doc body", "filename": "a.md"}, {"task_id": "t-1", "status": "queued"}),
    ("cortrix_list_documents", {}, {"documents": [{"id": "d1"}], "total": 1}),
    ("cortrix_list_namespaces", {}, {"namespaces": [{"name": "default"}], "total": 1}),
    ("cortrix_create_namespace", {"name": "team-a"}, {"name": "team-a"}),
    ("cortrix_memory_search", {"query": "past"}, {"memories": [{"id": "m1"}]}),
    ("cortrix_log_interaction", {"session_id": "s1", "query_text": "q", "response_text": "r"}, {"status": "ok"}),
    ("cortrix_list_interactions", {"namespace": "default", "user_id": "u1"}, {"interactions": [{"id": "i1"}], "total": 1}),
    ("cortrix_document_status", {"doc_id": "d1"}, {"id": "d1", "status": "ready"}),
    ("cortrix_add_watcher", {"data_dir": "__DIR__"}, {"id": "w1", "status": "active"}),
    ("cortrix_list_watchers", {}, {"watchers": [{"id": "w1"}]}),
    # --- extended 4 ---
    ("cortrix_cross_ns_query", {"query": "x", "namespaces": ["a", "b"]}, {"data": [], "meta": {"namespaces_failed": []}}),
    ("cortrix_async_upload", {"content": "big"}, {"task_id": "t-2", "status": "queued"}),
    ("cortrix_memory_search_filter", {"query": "x", "memory_type": "fact"}, {"memories": []}),
    ("cortrix_memory_extract_trigger", {"session_id": "s1"}, {"extracted": 3}),
    # --- new 4 ---
    ("cortrix_memory_extract", {"messages": [{"role": "user", "content": "hi"}]}, {"succeeded": [], "failed": []}),
    ("cortrix_task_status", {"task_id": "t-1"}, {"task_id": "t-1", "progress": 0.5}),
    ("cortrix_cancel_task", {"task_id": "t-1"}, {"task_id": "t-1", "status": "cancelled"}),
    ("cortrix_query_explain", {"query": "x"}, {"data": [], "meta": {"coverage_ratio": 0.8}}),
    # --- memory & ops 9 ---
    ("cortrix_memory_get_audit", {}, {"entries": [{"op": "create"}]}),
    ("cortrix_memory_revoke_fact", {"memory_id": "m1"}, {"memory_id": "m1", "auto_revoke_eligible": True}),
    ("cortrix_memory_opt_out", {"session_id": "s1"}, {"session_id": "s1", "opt_out": True}),
    ("cortrix_batch_submit", {"namespace": "ns", "documents": [{"doc_id": "x", "content": "c"}]},
     {"results": [{"doc_id": "x", "status": "ok"}], "meta": {"succeeded": 1, "failed": 0, "total": 1}}),
    ("cortrix_list_operations", {}, {"operations": [{"op_id": "o1"}], "total": 1, "has_more": False}),
    ("cortrix_memory_list", {}, {"memories": [{"memory_id": "m1"}], "total": 1}),
    ("cortrix_memory_create", {"namespace": "ns", "content": "c", "memory_type": "fact"},
     {"memory_id": "m2", "status": "active"}),
    ("cortrix_memory_edit", {"memory_id": "m1", "content": "new"},
     {"memory_id": "m1", "extraction_method": "user_edit"}),
    ("cortrix_memory_invalidate", {"memory_id": "m1", "reason": "outdated"},
     {"memory_id": "m1", "status": "invalidated"}),
]


@pytest.mark.parametrize("name,kwargs,body", SUCCESS_CASES, ids=[c[0] for c in SUCCESS_CASES])
def test_tool_success_envelope(name, kwargs, body, mock_request, tmp_path):
    # cortrix_add_watcher does a real os.path.isdir pre-check; give it a real dir.
    if kwargs.get("data_dir") == "__DIR__":
        kwargs = {**kwargs, "data_dir": str(tmp_path)}
    mock_request.set(json_body=body)
    out = get_tool_fn(name)()  if not kwargs else get_tool_fn(name)(**kwargs)
    assert_envelope(out)
    assert mock_request.client.request.called


def test_all_29_main_tools_have_a_success_case():
    """Guard: the success table covers exactly the 29 main tools (not admin)."""
    covered = {c[0] for c in SUCCESS_CASES}
    assert len(covered) == 29
    assert "cortrix_admin_db_credential_register" not in covered
    assert "cortrix_admin_db_import_run" not in covered


# ---------------------------------------------------------------------------
# Request shaping: verify P04-correct endpoints + bodies for representative tools.
# ---------------------------------------------------------------------------
def test_query_uses_namespaces_array_and_prefix(mock_request):
    # F04 wire (live since D3.5 r2): POST /query takes a `namespaces` array.
    mock_request.set(json_body={"data": []})
    get_tool_fn("cortrix_query")(query="hi", top_k=7, namespaces=["c1", "c2"], rerank=False)
    args, kwargs = mock_request.last_call
    assert args[0] == "POST"
    assert args[1].endswith("/api/v1/query")
    body = kwargs["json"]
    assert body["namespaces"] == ["c1", "c2"]
    assert body["top_k"] == 7
    assert body["rerank"] is False


def test_query_defaults_to_configured_namespace(mock_request):
    mock_request.set(json_body={"data": []})
    get_tool_fn("cortrix_query")(query="hi")
    body = mock_request.last_call.kwargs["json"]
    assert body["namespaces"] == [transport.CORTRIX_NAMESPACE]


def test_health_hits_system_health(mock_request):
    mock_request.set(json_body={"status": "ok"})
    get_tool_fn("cortrix_health")()
    args, _ = mock_request.last_call
    assert args[0] == "GET"
    # Backend exposes /system/health/{live,ready}; there is no bare /system/health.
    assert args[1].endswith("/api/v1/system/health/live")


def test_upload_posts_documents_with_namespace_in_body(mock_request):
    mock_request.set(json_body={"task_id": "t"})
    get_tool_fn("cortrix_upload")(content="x", namespace="ns", filename="f.txt")
    args, kwargs = mock_request.last_call
    assert args[1].endswith("/api/v1/documents")
    assert kwargs["json"]["namespace"] == "ns"
    assert kwargs["json"]["filename"] == "f.txt"


def test_add_watcher_posts_watch_with_target_namespaces(mock_request, tmp_path):
    mock_request.set(json_body={"id": "w"})
    get_tool_fn("cortrix_add_watcher")(data_dir=str(tmp_path), namespace="ns")
    args, kwargs = mock_request.last_call
    assert args[1].endswith("/api/v1/watch")
    assert kwargs["json"]["path"] == str(tmp_path)
    assert kwargs["json"]["target_namespaces"] == ["ns"]


def test_query_explain_sets_explain_param(mock_request):
    mock_request.set(json_body={"data": []})
    get_tool_fn("cortrix_query_explain")(query="hi")
    _, kwargs = mock_request.last_call
    assert kwargs["params"]["explain"] == "true"


def test_task_status_and_cancel_endpoints(mock_request):
    mock_request.set(json_body={"task_id": "t-9"})
    get_tool_fn("cortrix_task_status")(task_id="t-9")
    assert mock_request.last_call.args[1].endswith("/api/v1/documents/tasks/t-9/progress")
    mock_request.set(json_body={"task_id": "t-9", "status": "cancelled"})
    get_tool_fn("cortrix_cancel_task")(task_id="t-9")
    args, _ = mock_request.last_call
    assert args[0] == "DELETE"
    assert args[1].endswith("/api/v1/documents/tasks/t-9")


def test_memory_crud_endpoints(mock_request):
    mock_request.set(json_body={"memories": []})
    get_tool_fn("cortrix_memory_list")(memory_type="fact")
    assert mock_request.last_call.args[1].endswith("/api/v1/memory")
    assert mock_request.last_call.kwargs["params"]["memory_type"] == "fact"

    mock_request.set(json_body={"memory_id": "m"})
    get_tool_fn("cortrix_memory_create")(namespace="ns", content="c", memory_type="fact")
    assert mock_request.last_call.args[0] == "POST"

    mock_request.set(json_body={"memory_id": "m"})
    get_tool_fn("cortrix_memory_edit")(memory_id="m1", content="new", metadata={"k": "v"})
    args, kwargs = mock_request.last_call
    assert args[0] == "PATCH"
    assert args[1].endswith("/api/v1/memory/m1")
    assert kwargs["json"] == {"namespace": "default", "content": "new", "metadata": {"k": "v"}}

    mock_request.set(json_body={"memory_id": "m", "status": "invalidated"})
    get_tool_fn("cortrix_memory_invalidate")(memory_id="m1", reason="x")
    args, kwargs = mock_request.last_call
    assert args[0] == "DELETE"
    assert kwargs["params"]["reason"] == "x"


# ---------------------------------------------------------------------------
# Error paths.
# ---------------------------------------------------------------------------
def test_backend_timeout_maps_to_mcp_timeout(mock_request):
    mock_request.set_side_effect(httpx.TimeoutException("slow"))
    with pytest.raises(McpError) as ei:
        get_tool_fn("cortrix_health")()
    data = ei.value.error.data
    assert data["code"] == "CX_ERR_MCP_BACKEND_TIMEOUT"
    assert data["retryable"] is True
    assert data["category"] == "transient"
    assert data["retry_after_ms"] == 1000
    assert "structured_data" in data


def test_backend_connect_error_maps_to_unavailable(mock_request):
    mock_request.set_side_effect(httpx.ConnectError("refused"))
    with pytest.raises(McpError) as ei:
        get_tool_fn("cortrix_query")(query="x")
    data = ei.value.error.data
    assert data["code"] == "CX_ERR_MCP_BACKEND_UNAVAILABLE"
    assert data["retry_after_ms"] == 5000


def test_business_4xx_passes_through(mock_request):
    err_resp = make_response(
        status_code=403,
        json_body={"error": {"code": "CX_ERR_NS_UNAUTHORIZED", "retryable": False,
                              "category": "auth", "structured_data": {"ns": "secret"}}},
    )
    exc = httpx.HTTPStatusError("403", request=MagicMock(), response=err_resp)
    mock_request.set_side_effect(exc)
    with pytest.raises(McpError) as ei:
        get_tool_fn("cortrix_query")(query="x")
    data = ei.value.error.data
    assert data["code"] == "CX_ERR_NS_UNAUTHORIZED"  # business code not re-invented
    assert data["category"] == "auth"
    assert data["structured_data"]["ns"] == "secret"


def test_business_5xx_passes_through(mock_request):
    err_resp = make_response(
        status_code=500,
        json_body={"error": {"code": "CX_ERR_INTERNAL_ERROR", "retryable": True,
                              "category": "transient", "retry_after_ms": 200}},
    )
    exc = httpx.HTTPStatusError("500", request=MagicMock(), response=err_resp)
    mock_request.set_side_effect(exc)
    with pytest.raises(McpError) as ei:
        get_tool_fn("cortrix_list_namespaces")()
    assert ei.value.error.data["code"] == "CX_ERR_INTERNAL_ERROR"
    assert ei.value.error.data["retryable"] is True


def test_add_watcher_missing_dir_is_schema_validation_fail(mock_request):
    with pytest.raises(McpError) as ei:
        get_tool_fn("cortrix_add_watcher")(data_dir="/no/such/dir")
    assert ei.value.error.data["code"] == "CX_ERR_MCP_SCHEMA_VALIDATION_FAIL"
    assert ei.value.error.data["category"] == "permanent"


# ---------------------------------------------------------------------------
# errors.py / transport.py unit coverage.
# ---------------------------------------------------------------------------
def test_all_six_mcp_error_codes_constructible():
    for code in errors.MCP_ERROR_TABLE:
        err = errors.mcp_error(code)
        assert err.error.data["code"] == code
        assert err.error.data["category"] in errors.CATEGORY_ENUM


def test_unknown_mcp_code_raises_keyerror():
    with pytest.raises(KeyError):
        errors.mcp_error("CX_ERR_MCP_DOES_NOT_EXIST")


def test_success_envelope_shape():
    env = errors.success_envelope({"x": 1}, {"trace_id": "t"})
    assert env["data"] == {"x": 1}
    assert env["meta"]["category"] == "success"
    assert env["meta"]["structured_data"]["trace_id"] == "t"


def test_passthrough_defaults_when_body_empty():
    err = errors.passthrough_business_error(500, {})
    assert err.error.data["code"] == "CX_ERR_INTERNAL_ERROR"
    assert err.error.data["category"] == "permanent"


def test_session_id_stable_within_process():
    assert transport.mcp_session_id() == transport.mcp_session_id()
    assert transport.mcp_session_id().startswith("mcp-session-")


def test_request_204_returns_empty_envelope(mock_request):
    mock_request.set(status_code=204, json_body=None)
    out = transport.request("DELETE", "/watch/w1")
    assert out["data"] == {}
    assert out["meta"]["category"] == "success"


def test_structured_data_surfaces_business_meta(mock_request):
    mock_request.set(json_body={"data": [], "meta": {"coverage_ratio": 0.42, "namespaces_failed": ["x"]}})
    out = get_tool_fn("cortrix_query")(query="x")
    sd = out["meta"]["structured_data"]
    assert sd["coverage_ratio"] == 0.42
    assert sd["namespaces_failed"] == ["x"]
    assert sd["trace_id"] == "trace-test-1"
