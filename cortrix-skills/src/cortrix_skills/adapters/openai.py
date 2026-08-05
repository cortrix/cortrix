"""OpenAI Function Calling adapter — ``as_openai_functions(kit)``.

Converts a :class:`cortrix_skills.CortrixToolKit` into OpenAI Chat Completions
tool definitions (29 -> 29, feature design section 5.4-bis). Reuses the same
JSON Schema builder as the Claude adapter; the only differences are the
``{"type": "function", "function": {...}}`` envelope and the ``parameters`` key
(vs Claude's ``input_schema``). Building tool definitions needs **no** ``openai``
SDK.

:func:`dispatch_openai_tool_call` runs one OpenAI ``tool_call`` (whose
``arguments`` are a JSON-encoded string) and returns a JSON string suitable for
a ``role: "tool"`` message; a Python SDK ``CortrixError`` is encoded as the GEN-Agent
4-field JSON payload (error codes / fields passed through from Python SDK unchanged).

Real GPT round-trips are D3.5-deferred; standalone tests exercise the conversion
+ dispatch with a mocked toolkit. ``require_openai()`` lets callers fail fast
when the optional ``openai`` SDK is missing.
"""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any, Dict, List

from cortrix import CortrixError

from . import iter_descriptors

if TYPE_CHECKING:
    from ..toolkit import CortrixToolKit

_INSTALL_HINT = (
    "OpenAI real round-trip requires `pip install cortrix-skills[openai]` "
    "(openai>=1.0,<2.0). Building function definitions needs no openai SDK."
)


def require_openai() -> None:
    """Raise ImportError if the optional ``openai`` SDK is absent (real round-trip only)."""
    try:
        import openai  # noqa: F401
    except ImportError as exc:  # pragma: no cover - exercised via monkeypatch in tests
        raise ImportError(_INSTALL_HINT) from exc


def as_openai_functions(kit: "CortrixToolKit") -> List[Dict[str, Any]]:
    """Convert ``kit``'s 29 methods to OpenAI Chat Completions tool definitions.

    Returns a list of 29 dicts, each::

        {"type": "function",
         "function": {"name": "cortrix_query", "description": "...",
                      "parameters": {"type": "object", "properties": {...}, "required": [...]}}}

    directly usable as the Chat Completions ``tools`` parameter (gpt-4o /
    gpt-4o-mini / gpt-4-turbo / gpt-3.5-turbo).
    """
    return [
        {
            "type": "function",
            "function": {
                "name": desc.name,
                "description": desc.description,
                "parameters": desc.input_schema,
            },
        }
        for desc in iter_descriptors(kit)
    ]


def _error_payload(exc: CortrixError) -> Dict[str, Any]:
    return {
        "error": exc.error_code,
        "message": str(exc),
        "retryable": exc.retryable,
        "category": exc.category,
        "retry_after_ms": exc.retry_after_ms,
        "structured_data": exc.structured_data,
    }


def dispatch_openai_tool_call(kit: "CortrixToolKit", tool_call: Any) -> str:
    """Dispatch an OpenAI ``tool_call`` to the matching toolkit method.

    ``tool_call`` may be an OpenAI ``ChatCompletionMessageToolCall`` (with
    ``.function.name`` / ``.function.arguments``) or a plain dict::

        {"id": "...", "type": "function",
         "function": {"name": "cortrix_query", "arguments": '{"query": "..."}'}}

    Returns the JSON-encoded result string (for a ``role: "tool"`` message). A
    ``CortrixError`` returns the GEN-Agent 4-field JSON payload instead of
    raising (feature design section 5.4-bis).
    """
    if isinstance(tool_call, dict):
        function = tool_call["function"]
        name = function["name"]
        raw_args = function.get("arguments") or "{}"
    else:
        function = tool_call.function
        name = function.name
        raw_args = function.arguments or "{}"

    args = json.loads(raw_args) if isinstance(raw_args, str) else (raw_args or {})
    method = getattr(kit, name)
    try:
        return json.dumps(method(**args))
    except CortrixError as exc:
        return json.dumps(_error_payload(exc))
