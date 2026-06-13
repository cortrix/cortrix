"""Claude Tools adapter tests (mock; no anthropic SDK, no LLM round-trip).

Covers: 29 -> 29 tool definitions, schema shape (input_schema), 1:1 names with
the toolkit, tool_use dispatch (dict + object), and is_error=True + 4-field
passthrough on CortrixError (feature design section 5.4 / 6.3).
"""

from __future__ import annotations

import json

import pytest

from cortrix import CortrixError
from cortrix_skills import TOOL_METHOD_NAMES
from cortrix_skills.adapters.claude import (
    as_claude_tools,
    dispatch_claude_tool_use,
    require_anthropic,
)


def test_29_tool_definitions(kit):
    tools = as_claude_tools(kit)
    assert len(tools) == 29


def test_names_1to1_with_toolkit(kit):
    tools = as_claude_tools(kit)
    assert [t["name"] for t in tools] == list(TOOL_METHOD_NAMES)


def test_tool_definition_shape(kit):
    tools = as_claude_tools(kit)
    by_name = {t["name"]: t for t in tools}
    q = by_name["cortrix_query"]
    assert set(q.keys()) == {"name", "description", "input_schema"}
    assert q["description"]  # docstring first line present
    schema = q["input_schema"]
    assert schema["type"] == "object"
    assert "query" in schema["properties"]
    assert schema["required"] == ["query"]


def test_health_has_empty_properties(kit):
    tools = {t["name"]: t for t in as_claude_tools(kit)}
    assert tools["cortrix_health"]["input_schema"]["properties"] == {}


def test_dispatch_dict_tool_use_success(kit, fake_client):
    use = {"id": "tu_1", "name": "cortrix_query", "input": {"query": "hello"}}
    result = dispatch_claude_tool_use(kit, use)
    assert result["type"] == "tool_result"
    assert result["tool_use_id"] == "tu_1"
    assert "is_error" not in result
    payload = json.loads(result["content"])
    assert payload["results"][0]["child_id"] == "c1"


def test_dispatch_object_tool_use_success(kit):
    class ToolUse:
        id = "tu_2"
        name = "cortrix_health"
        input = {}

    result = dispatch_claude_tool_use(kit, ToolUse())
    assert result["tool_use_id"] == "tu_2"
    assert json.loads(result["content"])["status"] == "ok"


def test_dispatch_is_error_with_4_fields(kit, fake_client):
    def boom(*a, **k):
        raise CortrixError(
            "nope",
            error_code="CX_ERR_NAMESPACE_NOT_FOUND",
            retryable=False,
            category="permanent",
            retry_after_ms=None,
            structured_data={"namespace": "ghost"},
        )

    fake_client.system.health = boom
    result = dispatch_claude_tool_use(kit, {"id": "tu_3", "name": "cortrix_health", "input": {}})
    assert result["is_error"] is True
    payload = json.loads(result["content"])
    assert payload["code"] == "CX_ERR_NAMESPACE_NOT_FOUND"
    assert payload["retryable"] is False
    assert payload["category"] == "permanent"
    assert payload["retry_after_ms"] is None
    assert payload["structured_data"] == {"namespace": "ghost"}


def test_require_anthropic_raises_when_missing(monkeypatch):
    import builtins

    real_import = builtins.__import__

    def fake_import(name, *args, **kwargs):
        if name == "anthropic":
            raise ImportError("no anthropic")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_import)
    with pytest.raises(ImportError, match="cortrix-skills\\[claude\\]"):
        require_anthropic()
