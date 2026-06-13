"""OpenAI Function Calling adapter tests (mock; no openai SDK, no LLM round-trip).

Covers: 29 -> 29 function definitions, the {"type":"function","function":{...}}
envelope + "parameters" key, 1:1 names, tool_call dispatch (dict + object,
JSON-encoded arguments), and 4-field error JSON on CortrixError (feature design
section 5.4-bis).
"""

from __future__ import annotations

import json

import pytest

from cortrix import CortrixError
from cortrix_skills import TOOL_METHOD_NAMES
from cortrix_skills.adapters.openai import (
    as_openai_functions,
    dispatch_openai_tool_call,
    require_openai,
)


def test_29_function_definitions(kit):
    tools = as_openai_functions(kit)
    assert len(tools) == 29


def test_names_1to1_with_toolkit(kit):
    tools = as_openai_functions(kit)
    assert [t["function"]["name"] for t in tools] == list(TOOL_METHOD_NAMES)


def test_function_envelope_shape(kit):
    tools = as_openai_functions(kit)
    by_name = {t["function"]["name"]: t for t in tools}
    q = by_name["cortrix_query"]
    assert q["type"] == "function"
    fn = q["function"]
    assert set(fn.keys()) == {"name", "description", "parameters"}
    assert fn["parameters"]["type"] == "object"
    assert fn["parameters"]["required"] == ["query"]


def test_dispatch_dict_tool_call_success(kit):
    call = {
        "id": "call_1",
        "type": "function",
        "function": {"name": "cortrix_query", "arguments": json.dumps({"query": "hi", "top_k": 2})},
    }
    out = dispatch_openai_tool_call(kit, call)
    payload = json.loads(out)
    assert payload["results"][0]["child_id"] == "c1"


def test_dispatch_object_tool_call_success(kit):
    class Fn:
        name = "cortrix_health"
        arguments = "{}"

    class ToolCall:
        function = Fn()

    out = dispatch_openai_tool_call(kit, ToolCall())
    assert json.loads(out)["status"] == "ok"


def test_dispatch_empty_arguments(kit):
    call = {"function": {"name": "cortrix_health", "arguments": ""}}
    out = dispatch_openai_tool_call(kit, call)
    assert json.loads(out)["status"] == "ok"


def test_dispatch_error_4_fields(kit, fake_client):
    fake_client.set_http_error(
        CortrixError(
            "boom",
            error_code="CX_ERR_RATE_LIMITED",
            retryable=True,
            category="quota",
            retry_after_ms=2000,
            structured_data={"limit": 5},
        )
    )
    call = {"function": {"name": "cortrix_memory_search", "arguments": json.dumps({"query": "q"})}}
    out = dispatch_openai_tool_call(kit, call)
    payload = json.loads(out)
    assert payload["error"] == "CX_ERR_RATE_LIMITED"
    assert payload["retryable"] is True
    assert payload["category"] == "quota"
    assert payload["retry_after_ms"] == 2000
    assert payload["structured_data"] == {"limit": 5}


def test_require_openai_raises_when_missing(monkeypatch):
    import builtins

    real_import = builtins.__import__

    def fake_import(name, *args, **kwargs):
        if name == "openai":
            raise ImportError("no openai")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_import)
    with pytest.raises(ImportError, match="cortrix-skills\\[openai\\]"):
        require_openai()
