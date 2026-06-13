"""Tests for the ``cortrix_skills.adapters`` package facade.

The package-level ``as_*`` helpers lazily delegate to the per-framework
submodules (so importing the package never requires a framework). These tests
exercise that public surface directly (the documented import path in the
README / feature design section 8.2).
"""

from __future__ import annotations

import json
import sys
import types

import pytest

from cortrix import CortrixError
from cortrix_skills import TOOL_METHOD_NAMES
from cortrix_skills import adapters


def test_iter_descriptors_yields_29_in_order(kit):
    descs = list(adapters.iter_descriptors(kit))
    assert [d.name for d in descs] == list(TOOL_METHOD_NAMES)
    assert all(d.input_schema["type"] == "object" for d in descs)


def test_facade_as_claude_tools(kit):
    tools = adapters.as_claude_tools(kit)
    assert len(tools) == 29 and tools[0]["name"] == TOOL_METHOD_NAMES[0]


def test_facade_as_openai_functions(kit):
    tools = adapters.as_openai_functions(kit)
    assert len(tools) == 29 and tools[0]["type"] == "function"


def test_facade_dispatch_openai_tool_call(kit):
    call = {"function": {"name": "cortrix_health", "arguments": "{}"}}
    out = adapters.dispatch_openai_tool_call(kit, call)
    assert json.loads(out)["status"] == "ok"


def test_facade_as_langchain_tools(kit, monkeypatch):
    # Inject a minimal fake langchain so the facade path is exercised.
    class _ToolException(Exception):
        pass

    class _StructuredTool:
        def __init__(self, **kw):
            self.__dict__.update(kw)

        @classmethod
        def from_function(cls, **kw):
            return cls(**kw)

    langchain = types.ModuleType("langchain")
    langchain.__version__ = "0.2.16"
    tools_mod = types.ModuleType("langchain.tools")
    tools_mod.StructuredTool = _StructuredTool
    tools_mod.ToolException = _ToolException
    monkeypatch.setitem(sys.modules, "langchain", langchain)
    monkeypatch.setitem(sys.modules, "langchain.tools", tools_mod)

    tools = adapters.as_langchain_tools(kit)
    assert len(tools) == 29
