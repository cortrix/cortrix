"""MCP protocol-level checks — what an IDE (Claude Code / Cline / Cursor) sees on connect.

D3.5 deferred: the full real-machine round-trip (live cortrix-server + IDE stdio handshake)
runs during integration. Standalone, we validate the discoverable surface: tool count,
JSON input schemas, names, and server identity — i.e. what tools/list returns.
"""

from __future__ import annotations

import asyncio

import pytest
from conftest import mcp
from cortrix_mcp import __version__

# Expected 29 main tools (feature design section 4) + 2 admin tools (section 4.6).
MAIN_TOOLS = {
    # core 12
    "cortrix_health", "cortrix_query", "cortrix_upload", "cortrix_list_documents",
    "cortrix_list_namespaces", "cortrix_create_namespace", "cortrix_memory_search",
    "cortrix_log_interaction", "cortrix_list_interactions", "cortrix_document_status",
    "cortrix_add_watcher", "cortrix_list_watchers",
    # extended 4
    "cortrix_cross_ns_query", "cortrix_async_upload", "cortrix_memory_search_filter",
    "cortrix_memory_extract_trigger",
    # new 4
    "cortrix_memory_extract", "cortrix_task_status", "cortrix_cancel_task",
    "cortrix_query_explain",
    # Memory extraction +2 / memory opt-out +1 / BULK +1 / operation log +1
    "cortrix_memory_get_audit", "cortrix_memory_revoke_fact", "cortrix_memory_opt_out",
    "cortrix_batch_submit", "cortrix_list_operations",
    # Memory transparency +4
    "cortrix_memory_list", "cortrix_memory_create", "cortrix_memory_edit",
    "cortrix_memory_invalidate",
}
ADMIN_TOOLS = {"cortrix_admin_db_credential_register", "cortrix_admin_db_import_run"}


@pytest.fixture(scope="module")
def listed_tools():
    return asyncio.run(mcp.list_tools())


def test_server_identity():
    assert mcp.name == "cortrix"
    assert mcp.version == __version__


def test_total_tool_count_is_31(listed_tools):
    assert len(listed_tools) == 31  # 29 main + 2 admin


def test_exact_tool_names(listed_tools):
    names = {t.name for t in listed_tools}
    assert names == MAIN_TOOLS | ADMIN_TOOLS


def test_main_tool_count_is_29(listed_tools):
    names = {t.name for t in listed_tools}
    assert len(names & MAIN_TOOLS) == 29


def test_every_tool_has_input_schema(listed_tools):
    """MCPServer must emit a JSON Schema for each tool."""
    for t in listed_tools:
        assert t.input_schema is not None
        assert t.input_schema.get("type") == "object"
        assert "properties" in t.input_schema


def test_every_tool_has_description(listed_tools):
    for t in listed_tools:
        assert t.description and t.description.strip()


def test_tool_names_use_flat_cortrix_prefix(listed_tools):
    """Topic 4: flat cortrix_<action> naming."""
    for t in listed_tools:
        assert t.name.startswith("cortrix_")


def test_required_params_present_in_schema(listed_tools):
    """Spot-check that a required positional (no default) shows up as required."""
    by_name = {t.name: t for t in listed_tools}
    # cortrix_query.query has no default -> required.
    assert "query" in by_name["cortrix_query"].input_schema.get("required", [])
    # cortrix_memory_create requires namespace + content + memory_type.
    create_req = set(by_name["cortrix_memory_create"].input_schema.get("required", []))
    assert {"namespace", "content", "memory_type"} <= create_req
