"""Server instructions and the search-first guidance agents see before their first call."""

from __future__ import annotations

import anyio
from cortrix_mcp import server

# Claude Code keeps the first 2,048 characters of a server's instructions and of each
# tool description (CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH); anything past that is cut.
CLIENT_LIMIT = 2048


def _tools():
    return {t.name: t for t in anyio.run(server.mcp.list_tools)}


def test_server_publishes_the_instructions():
    assert server.mcp.instructions == server.INSTRUCTIONS


def test_instructions_fit_the_client_limit():
    assert len(server.INSTRUCTIONS) <= CLIENT_LIMIT


def test_instructions_name_the_default_namespace_and_search_first():
    assert f'"{server.CORTRIX_NAMESPACE}" namespace' in server.INSTRUCTIONS
    assert "search before you read" in server.INSTRUCTIONS
    for tool in ("cortrix_query", "cortrix_list_namespaces", "cortrix_list_documents"):
        assert tool in server.INSTRUCTIONS


def test_tool_descriptions_fit_the_client_limit():
    assert all(len(t.description or "") <= CLIENT_LIMIT for t in _tools().values())


def test_query_description_says_search_before_reading():
    description = _tools()["cortrix_query"].description
    assert "Search before you read" in description
    assert "metadata" in description
