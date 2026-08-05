"""Framework adapters for CortrixToolKit.

Each adapter converts a :class:`cortrix_skills.CortrixToolKit` into one
framework's tool representation:

  * :func:`as_langchain_tools`   -> list[langchain.tools.StructuredTool]
  * :func:`as_claude_tools`      -> list[dict]  (Anthropic Messages API ``tools``)
  * :func:`as_openai_functions`  -> list[dict]  (OpenAI Chat Completions ``tools``)

The framework imports are soft: the ``as_*`` helpers are imported lazily here so
that ``import cortrix_skills.adapters`` does not require langchain / anthropic /
openai. A missing framework only fails when its specific ``as_*`` helper runs
(feature design section 5 issue 4 / section 12.x: ImportError with an install
hint).

``iter_descriptors(kit)`` yields the 29 :class:`ToolDescriptor` objects in the
canonical MCP server tool order; all three adapters build on it.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Iterator

from ..descriptor import ToolDescriptor, describe_method
from ..toolkit import TOOL_METHOD_NAMES

if TYPE_CHECKING:  # avoid importing the toolkit type at runtime for adapters
    from ..toolkit import CortrixToolKit

__all__ = [
    "iter_descriptors",
    "as_langchain_tools",
    "as_claude_tools",
    "as_openai_functions",
    "dispatch_openai_tool_call",
]


def iter_descriptors(kit: "CortrixToolKit") -> Iterator[ToolDescriptor]:
    """Yield a :class:`ToolDescriptor` for each of the 29 toolkit methods.

    Iterates :data:`TOOL_METHOD_NAMES` (deterministic MCP server tool order) rather than
    ``dir(kit)`` so the 1:1 count and ordering are explicit and stable.
    """
    for name in TOOL_METHOD_NAMES:
        method = getattr(kit, name)
        yield describe_method(method)


def as_langchain_tools(kit: "CortrixToolKit"):
    """Convert a CortrixToolKit to LangChain tools (requires ``[langchain]``)."""
    from .langchain import as_langchain_tools as _impl

    return _impl(kit)


def as_claude_tools(kit: "CortrixToolKit"):
    """Convert a CortrixToolKit to Claude tool definitions (requires ``[claude]``)."""
    from .claude import as_claude_tools as _impl

    return _impl(kit)


def as_openai_functions(kit: "CortrixToolKit"):
    """Convert a CortrixToolKit to OpenAI function definitions (requires ``[openai]``)."""
    from .openai import as_openai_functions as _impl

    return _impl(kit)


def dispatch_openai_tool_call(kit: "CortrixToolKit", tool_call):
    """Dispatch an OpenAI ``tool_call`` to the matching toolkit method."""
    from .openai import dispatch_openai_tool_call as _impl

    return _impl(kit, tool_call)
