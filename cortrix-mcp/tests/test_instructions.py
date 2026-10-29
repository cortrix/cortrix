"""Server instructions and the search-first guidance agents see before their first call."""

from __future__ import annotations

import anyio
import pytest

from cortrix_mcp import server

# Claude Code keeps the first 2,048 characters of a server's instructions and of each
# tool description (CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH); anything past that is cut.
CLIENT_LIMIT = 2048


def _tools():
    return {t.name: t for t in anyio.run(server.mcp.list_tools)}


def test_server_publishes_the_instructions():
    assert server.mcp.instructions == server.INSTRUCTIONS


def test_instructions_follow_the_configured_namespace():
    assert server.INSTRUCTIONS == server.build_instructions(server.CORTRIX_NAMESPACE)


@pytest.mark.parametrize("namespace", ["default", "*"])
def test_instructions_fit_the_client_limit(namespace):
    assert len(server.build_instructions(namespace)) <= CLIENT_LIMIT


@pytest.mark.parametrize("namespace", ["default", "*"])
def test_instructions_say_search_first(namespace):
    text = server.build_instructions(namespace)
    assert "search before you read" in text
    for tool in ("cortrix_query", "cortrix_list_namespaces", "cortrix_list_documents"):
        assert tool in text


def test_instructions_name_a_single_default_namespace():
    text = server.build_instructions("docs")
    assert '"docs" namespace unless you pass `namespaces`' in text


def test_instructions_ask_for_a_namespace_when_the_default_is_every_namespace():
    text = server.build_instructions("*")
    assert '"*" namespace' not in text
    assert "searches every namespace" in text
    assert "pass the namespace that holds the documents you need" in text
    assert "namespaces_failed" in text


def test_tool_descriptions_fit_the_client_limit():
    assert all(len(t.description or "") <= CLIENT_LIMIT for t in _tools().values())


def test_query_description_says_search_before_reading():
    description = _tools()["cortrix_query"].description
    assert "Search before you read" in description
    assert "metadata" in description
