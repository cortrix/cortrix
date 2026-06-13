"""LangChain adapter tests (mock; a fake ``langchain`` module is injected).

Real LangChain ReAct round-trips against a live LLM are D3.5-deferred and not
run here. We inject a minimal fake ``langchain`` (StructuredTool + ToolException)
into ``sys.modules`` to exercise: 29 -> 29 conversion, name/description/args_schema
wiring, CortrixError -> ToolException(4-field JSON), the missing-dependency
ImportError, and the version guard (only 0.2.x supported).
"""

from __future__ import annotations

import json
import sys
import types

import pytest

from cortrix import CortrixError
from cortrix_skills import TOOL_METHOD_NAMES


# --- fake langchain module ---------------------------------------------------

class _FakeToolException(Exception):
    pass


class _FakeStructuredTool:
    def __init__(self, func, name, description, args_schema, handle_tool_error):
        self.func = func
        self.name = name
        self.description = description
        self.args_schema = args_schema
        self.handle_tool_error = handle_tool_error

    @classmethod
    def from_function(cls, *, func, name, description, args_schema, handle_tool_error):
        return cls(func, name, description, args_schema, handle_tool_error)

    def run(self, **kwargs):
        return self.func(**kwargs)


def _install_fake_langchain(monkeypatch, version="0.2.16"):
    langchain = types.ModuleType("langchain")
    langchain.__version__ = version
    tools_mod = types.ModuleType("langchain.tools")
    tools_mod.StructuredTool = _FakeStructuredTool
    tools_mod.ToolException = _FakeToolException
    monkeypatch.setitem(sys.modules, "langchain", langchain)
    monkeypatch.setitem(sys.modules, "langchain.tools", tools_mod)


@pytest.fixture
def fake_langchain(monkeypatch):
    _install_fake_langchain(monkeypatch)
    yield


# --- tests -------------------------------------------------------------------

def test_29_langchain_tools(kit, fake_langchain):
    from cortrix_skills.adapters.langchain import as_langchain_tools

    tools = as_langchain_tools(kit)
    assert len(tools) == 29
    assert [t.name for t in tools] == list(TOOL_METHOD_NAMES)


def test_tool_wiring(kit, fake_langchain):
    from cortrix_skills.adapters.langchain import as_langchain_tools

    tools = {t.name: t for t in as_langchain_tools(kit)}
    q = tools["cortrix_query"]
    assert q.description
    assert q.handle_tool_error is True
    # args_schema is the pydantic model derived from the signature.
    fields = q.args_schema.model_fields
    assert "query" in fields and "top_k" in fields


def test_tool_invocation_success(kit, fake_langchain):
    from cortrix_skills.adapters.langchain import as_langchain_tools

    tools = {t.name: t for t in as_langchain_tools(kit)}
    out = tools["cortrix_query"].run(query="hi")
    assert out["results"][0]["child_id"] == "c1"


def test_tool_error_becomes_toolexception_with_4_fields(kit, fake_client, fake_langchain):
    from cortrix_skills.adapters.langchain import as_langchain_tools

    def boom(*a, **k):
        raise CortrixError(
            "rl",
            error_code="CX_ERR_RATE_LIMITED",
            retryable=True,
            category="quota",
            retry_after_ms=1000,
            structured_data={"x": 1},
        )

    fake_client.system.health = boom
    tools = {t.name: t for t in as_langchain_tools(kit)}
    with pytest.raises(_FakeToolException) as ei:
        tools["cortrix_health"].run()
    payload = json.loads(str(ei.value))
    assert payload["code"] == "CX_ERR_RATE_LIMITED"
    assert payload["retryable"] is True
    assert payload["category"] == "quota"
    assert payload["retry_after_ms"] == 1000
    assert payload["structured_data"] == {"x": 1}


def test_missing_langchain_raises_importerror(kit, monkeypatch):
    # Ensure langchain is absent, then force the import to fail.
    monkeypatch.setitem(sys.modules, "langchain", None)
    from cortrix_skills.adapters.langchain import as_langchain_tools

    with pytest.raises(ImportError, match="cortrix-skills\\[langchain\\]"):
        as_langchain_tools(kit)


def test_version_guard_rejects_non_0_2(kit, monkeypatch):
    _install_fake_langchain(monkeypatch, version="0.3.1")
    from cortrix_skills.adapters.langchain import as_langchain_tools

    with pytest.raises(RuntimeError, match="langchain 0.2.x"):
        as_langchain_tools(kit)
