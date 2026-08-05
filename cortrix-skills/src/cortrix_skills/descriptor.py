"""Tool descriptor generation: CortrixToolKit method signature -> pydantic model
-> JSON Schema.

This module is framework-agnostic. Every adapter (LangChain / Claude Tools /
OpenAI Function Calling) reuses ``json_schema_from_method`` /
``pydantic_model_from_method`` so the schema a model sees is identical across
frameworks (feature design section 5, issue 2: pydantic BaseModel + auto JSON
Schema).

The toolkit methods carry plain Python type hints + defaults; we reflect over
the signature and synthesize a ``pydantic.BaseModel`` whose fields mirror the
parameters. Pydantic v2 then renders a JSON Schema via ``model_json_schema()``.
"""

from __future__ import annotations

import inspect
import re
import typing
from typing import Any, Callable

from pydantic import Field, create_model

__all__ = [
    "ToolDescriptor",
    "pydantic_model_from_method",
    "json_schema_from_method",
    "describe_method",
    "first_line",
]

# Pydantic appends "Input" to synthesized arg-schema model names for readability
# (e.g. CortrixQueryInput).
_MODEL_SUFFIX = "Input"


def first_line(doc: str | None) -> str:
    """Return the first non-empty line of a docstring, stripped.

    Used as the short ``description`` a model sees in the tool list.
    """
    if not doc:
        return ""
    for line in doc.strip().splitlines():
        stripped = line.strip()
        if stripped:
            return stripped
    return ""


def _model_name(method_name: str) -> str:
    """Turn ``cortrix_query`` into ``CortrixQueryInput`` (a valid model name)."""
    parts = re.split(r"[_\s]+", method_name)
    camel = "".join(p[:1].upper() + p[1:] for p in parts if p)
    return f"{camel}{_MODEL_SUFFIX}"


def pydantic_model_from_method(method: Callable[..., Any]):
    """Build a ``pydantic.BaseModel`` subclass mirroring ``method``'s parameters.

    ``self`` is skipped. ``*args`` / ``**kwargs`` are ignored (none of the 29
    methods use them). A parameter without a default becomes a required field;
    a parameter with a default becomes optional with that default. Parameters
    without a type annotation fall back to ``Any``.

    The toolkit uses ``from __future__ import annotations`` (PEP 563), so raw
    parameter annotations are *strings*. We resolve them to real types with
    :func:`typing.get_type_hints` (evaluated in the method's module globals) so
    pydantic can build the field schema (otherwise ``Optional`` / ``List`` would
    be unresolved forward refs).
    """
    sig = inspect.signature(method)
    try:
        hints = typing.get_type_hints(method)
    except Exception:  # pragma: no cover - defensive; falls back to Any per-field
        hints = {}
    fields: dict[str, tuple[Any, Any]] = {}
    for name, param in sig.parameters.items():
        if name == "self":
            continue
        if param.kind in (inspect.Parameter.VAR_POSITIONAL, inspect.Parameter.VAR_KEYWORD):
            continue
        annotation = hints.get(name, Any)
        if param.default is inspect.Parameter.empty:
            fields[name] = (annotation, Field(...))
        else:
            fields[name] = (annotation, Field(default=param.default))
    return create_model(_model_name(method.__name__), **fields)  # type: ignore[call-overload]


def json_schema_from_method(method: Callable[..., Any]) -> dict[str, Any]:
    """Return a JSON Schema (object) describing ``method``'s parameters.

    The shape is the standard pydantic v2 ``model_json_schema()`` output:
    ``{"type": "object", "properties": {...}, "required": [...]}`` — directly
    usable as a Claude ``input_schema`` or an OpenAI function ``parameters``
    block.
    """
    model = pydantic_model_from_method(method)
    schema = model.model_json_schema()
    # Synthesized model titles ("CortrixQueryInput") leak the internal name; drop
    # it so the schema reads as a plain parameter object.
    schema.pop("title", None)
    return schema


class ToolDescriptor:
    """Framework-neutral description of one CortrixToolKit method.

    Attributes:
        name: the method name (``cortrix_<action>``), used verbatim as the
            framework tool name (the MCP server ``cortrix_<action>`` naming SoT).
        description: first line of the method docstring.
        input_schema: JSON Schema for the parameters.
        method: the bound callable (so an adapter can invoke it).
    """

    __slots__ = ("name", "description", "input_schema", "method")

    def __init__(self, name: str, description: str, input_schema: dict[str, Any], method: Callable[..., Any]):
        self.name = name
        self.description = description
        self.input_schema = input_schema
        self.method = method

    def __repr__(self) -> str:
        return f"ToolDescriptor(name={self.name!r})"


def describe_method(method: Callable[..., Any]) -> ToolDescriptor:
    """Build a :class:`ToolDescriptor` for a single bound toolkit method."""
    return ToolDescriptor(
        name=method.__name__,
        description=first_line(method.__doc__),
        input_schema=json_schema_from_method(method),
        method=method,
    )
