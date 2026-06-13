"""LangChain adapter — ``as_langchain_tools(kit)``.

Converts a :class:`cortrix_skills.CortrixToolKit` into LangChain
``StructuredTool`` objects (29 -> 29). Each tool's ``args_schema`` is the pydantic
model derived from the method signature (feature design section 5.3), and each
call is wrapped so a P03 ``CortrixError`` is re-raised as a LangChain
``ToolException`` whose message is the GEN-Agent 4-field JSON payload (feature
design section 6.2). The error codes / 4 fields are passed through from P03
unchanged — no re-wrapping of the values, only of the carrier exception.

Soft dependency: ``langchain`` is imported here, not in the package root.
Missing or wrong-version langchain raises a clear error with an install hint
(feature design section 12.x). Real LangChain ReAct round-trips against a live
LLM are D3.5-deferred; standalone tests exercise the conversion + error wrapping
with a mocked toolkit.
"""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any, List

from cortrix import CortrixError

from ..descriptor import pydantic_model_from_method
from . import iter_descriptors

if TYPE_CHECKING:
    from ..toolkit import CortrixToolKit

_INSTALL_HINT = (
    "LangChain adapter requires `pip install cortrix-skills[langchain]` "
    "(langchain>=0.2.0,<0.3.0)."
)


def _import_langchain():
    """Import LangChain's Tool primitives, with a version guard.

    Returns ``(StructuredTool, ToolException)``. Raises ``ImportError`` when
    langchain is absent and ``RuntimeError`` when an unsupported major/minor is
    installed (feature design section 12.x version-incompatibility row).
    """
    try:
        import langchain
        from langchain.tools import StructuredTool, ToolException
    except ImportError as exc:  # pragma: no cover - exercised via monkeypatch in tests
        raise ImportError(_INSTALL_HINT) from exc

    version = getattr(langchain, "__version__", "0")
    parts = version.split(".")
    try:
        major, minor = int(parts[0]), int(parts[1])
    except (IndexError, ValueError):
        major = minor = -1
    if (major, minor) != (-1, -1) and not (major == 0 and minor == 2):
        raise RuntimeError(
            f"cortrix-skills LangChain adapter requires langchain 0.2.x; found {version}."
        )
    return StructuredTool, ToolException


def _wrap(method: Any, ToolException: Any):
    """Wrap a toolkit method so CortrixError -> ToolException(4-field JSON)."""

    def _runner(**kwargs: Any) -> Any:
        try:
            return method(**kwargs)
        except CortrixError as exc:
            raise ToolException(
                json.dumps(
                    {
                        "code": exc.error_code,
                        "message": str(exc),
                        "retryable": exc.retryable,
                        "category": exc.category,
                        "retry_after_ms": exc.retry_after_ms,
                        "structured_data": exc.structured_data,
                    }
                )
            ) from exc

    return _runner


def as_langchain_tools(kit: "CortrixToolKit") -> List[Any]:
    """Convert ``kit``'s 29 methods to LangChain ``StructuredTool`` objects.

    Returns a list of 29 tools (one per CortrixToolKit method). Each carries the
    method name, its docstring first line as ``description``, the pydantic
    ``args_schema``, and ``handle_tool_error=True`` so the framework surfaces the
    4-field error payload back to the agent loop.
    """
    StructuredTool, ToolException = _import_langchain()
    tools: List[Any] = []
    for desc in iter_descriptors(kit):
        args_schema = pydantic_model_from_method(desc.method)
        tools.append(
            StructuredTool.from_function(
                func=_wrap(desc.method, ToolException),
                name=desc.name,
                description=desc.description,
                args_schema=args_schema,
                handle_tool_error=True,
            )
        )
    return tools
