"""Read-only mode registers only the tools that leave the store unchanged."""

from __future__ import annotations

import anyio

from cortrix_mcp import server
from cortrix_mcp.mcp_server import READ_ONLY_TOOLS, CortrixMCPServer
from cortrix_mcp.tools import register_all


def _names(mcp):
    return {t.name for t in anyio.run(mcp.list_tools)}


def test_default_server_registers_every_tool():
    assert READ_ONLY_TOOLS < _names(server.mcp)
    assert len(_names(server.mcp)) == 32


def test_read_only_server_registers_only_the_read_tools():
    mcp = CortrixMCPServer("cortrix-test", version="0", instructions="", read_only=True)
    register_all(mcp)
    assert _names(mcp) == set(READ_ONLY_TOOLS)


def test_read_only_tools_are_a_subset_of_the_default_set_with_no_writer():
    writers = {"cortrix_upload", "cortrix_async_upload", "cortrix_batch_submit", "cortrix_create_namespace",
               "cortrix_add_watcher", "cortrix_log_interaction", "cortrix_cancel_task", "cortrix_memory_create",
               "cortrix_memory_edit", "cortrix_memory_invalidate", "cortrix_memory_extract",
               "cortrix_memory_extract_trigger", "cortrix_memory_revoke_fact", "cortrix_memory_opt_out",
               "cortrix_admin_db_credential_register", "cortrix_admin_db_import_run"}
    assert not writers & READ_ONLY_TOOLS
    assert writers | READ_ONLY_TOOLS == _names(server.mcp)


def test_instructions_say_so_in_read_only_mode():
    text = server.build_instructions("docs", read_only=True)
    assert "runs read-only" in text and "nothing here writes to the store" in text
    assert "call them only when the user asks" not in text
    assert len(text) <= 2048
    assert "call them only when the user asks" in server.build_instructions("docs")
