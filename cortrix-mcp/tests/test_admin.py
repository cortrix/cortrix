"""Tests for the 2 F16a admin-scope tools + the admin gating in transport.require_admin.

Admin tools require role=admin; V1.0 fallback is the CORTRIX_MCP_ADMIN env var (parsed
into transport.CORTRIX_MCP_ADMIN at import). We monkeypatch that module flag to flip the
gate without re-importing.
"""

from __future__ import annotations

import pytest
from conftest import call_tool_result, get_tool_fn

from cortrix_mcp import transport
from cortrix_mcp.errors import CortrixToolError

ADMIN_TOOLS = ["cortrix_admin_db_credential_register", "cortrix_admin_db_import_run"]


@pytest.fixture
def admin_on(monkeypatch):
    monkeypatch.setattr(transport, "CORTRIX_MCP_ADMIN", True)


@pytest.fixture
def admin_off(monkeypatch):
    monkeypatch.setattr(transport, "CORTRIX_MCP_ADMIN", False)


@pytest.mark.parametrize("name", ADMIN_TOOLS)
def test_admin_tool_denied_without_role(name, admin_off, mock_request):
    kwargs = (
        {"name": "pg1", "dsn": "postgres://x"}
        if name == "cortrix_admin_db_credential_register"
        else {"connection_ref": "pg1", "namespace": "ns", "table": "t"}
    )
    result = call_tool_result(name, **kwargs)
    assert result.is_error is True
    data = result.structured_content
    assert data["code"] == "CX_ERR_MCP_ADMIN_REQUIRED"
    assert data["category"] == "auth"
    assert data["retryable"] is False
    # Denied before any HTTP call is made.
    assert not mock_request.client.request.called


def test_credential_register_success(admin_on, mock_request):
    mock_request.set(json_body={"ref_id": "ref-1", "name": "pg1"})
    out = get_tool_fn("cortrix_admin_db_credential_register")(
        name="pg1", dsn="postgres://u:p@h/db", expire_days=30
    )
    assert out["meta"]["category"] == "success"
    assert out["data"]["name"] == "pg1"
    args, kwargs = mock_request.last_call
    assert args[0] == "POST"
    # Backend route is /api/v1/admin/db-connections; body is {name, dsn, expire_days?}.
    assert args[1].endswith("/api/v1/admin/db-connections")
    assert kwargs["json"]["name"] == "pg1"
    assert kwargs["json"]["dsn"] == "postgres://u:p@h/db"
    assert kwargs["json"]["expire_days"] == 30
    assert "connection_ref" not in kwargs["json"]
    assert "description" not in kwargs["json"]


def test_db_import_run_table_mode(admin_on, mock_request):
    mock_request.set(json_body={"import_id": "imp-1", "status": "running"})
    filter_body = {"where": [{"column": "active", "op": "eq", "value": True}]}
    out = get_tool_fn("cortrix_admin_db_import_run")(
        connection_ref="pg1", namespace="ns", table="users", filter=filter_body
    )
    assert out["meta"]["category"] == "success"
    body = mock_request.last_call.kwargs["json"]
    assert args_endswith(mock_request, "/api/v1/import/database")
    assert body["table"] == "users"
    assert body["filter"] == filter_body
    assert "sql" not in body


def test_db_import_run_sql_mode(admin_on, mock_request):
    mock_request.set(json_body={"import_id": "imp-2", "status": "running"})
    get_tool_fn("cortrix_admin_db_import_run")(
        connection_ref="pg1", namespace="ns", sql="SELECT * FROM t"
    )
    body = mock_request.last_call.kwargs["json"]
    assert body["sql"] == "SELECT * FROM t"
    assert "table" not in body


def test_require_admin_passes_when_flag_true(admin_on):
    # Should not raise.
    transport.require_admin()


def test_require_admin_raises_when_flag_false(admin_off):
    with pytest.raises(CortrixToolError) as ei:
        transport.require_admin()
    assert ei.value.data["code"] == "CX_ERR_MCP_ADMIN_REQUIRED"


def args_endswith(mock_request, suffix):
    return mock_request.last_call.args[1].endswith(suffix)
