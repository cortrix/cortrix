"""Claude Tools adapter — ``as_claude_tools(kit)``.

Converts a :class:`cortrix_skills.CortrixToolKit` into Anthropic Messages API
tool definitions (29 -> 29 dicts of ``{name, description, input_schema}``,
feature design section 5.4). Because Claude tool definitions are plain JSON, the
schema builder needs **no** ``anthropic`` SDK at all.

For the agent loop, :func:`dispatch_claude_tool_use` runs one ``tool_use`` block
and returns the matching ``tool_result`` block; a Python SDK ``CortrixError`` becomes
``is_error=True`` + the GEN-Agent 4-field JSON content (feature design section
6.3). Error codes / 4 fields are passed through from the Python SDK unchanged.

``require_anthropic()`` is provided for callers that want to fail fast when the
optional ``anthropic`` SDK (needed only for the real Messages round-trip, which
is D3.5-deferred) is not installed; the schema/dispatch helpers themselves do
not import it.
"""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any, Dict, List

from cortrix import CortrixError

from . import iter_descriptors

if TYPE_CHECKING:
    from ..toolkit import CortrixToolKit

_INSTALL_HINT = (
    "Claude Tools real round-trip requires `pip install cortrix-skills[claude]` "
    "(anthropic>=0.40.0,<1.0.0). Building tool definitions needs no anthropic SDK."
)


def require_anthropic() -> None:
    """Raise ImportError if the optional ``anthropic`` SDK is absent.

    Building tool definitions (``as_claude_tools``) and dispatching tool_use
    blocks do not need the SDK; this is for callers that drive a real Messages
    round-trip (D3.5).
    """
    try:
        import anthropic  # noqa: F401
    except ImportError as exc:  # pragma: no cover - exercised via monkeypatch in tests
        raise ImportError(_INSTALL_HINT) from exc


def as_claude_tools(kit: "CortrixToolKit") -> List[Dict[str, Any]]:
    """Convert ``kit``'s 29 methods to Claude tool definitions.

    Returns a list of 29 dicts, each::

        {"name": "cortrix_query", "description": "...",
         "input_schema": {"type": "object", "properties": {...}, "required": [...]}}

    directly usable as the Anthropic Messages API ``tools`` parameter.
    """
    return [
        {
            "name": desc.name,
            "description": desc.description,
            "input_schema": desc.input_schema,
        }
        for desc in iter_descriptors(kit)
    ]


def _error_payload(exc: CortrixError) -> Dict[str, Any]:
    return {
        "code": exc.error_code,
        "message": str(exc),
        "retryable": exc.retryable,
        "category": exc.category,
        "retry_after_ms": exc.retry_after_ms,
        "structured_data": exc.structured_data,
    }


def dispatch_claude_tool_use(kit: "CortrixToolKit", tool_use: Any) -> Dict[str, Any]:
    """Run one Claude ``tool_use`` block and return its ``tool_result`` block.

    ``tool_use`` may be an Anthropic ``ToolUseBlock`` (attributes ``id`` /
    ``name`` / ``input``) or a plain dict with the same keys. On success the
    result is JSON-encoded into ``content``; a ``CortrixError`` produces
    ``is_error=True`` + the GEN-Agent 4-field JSON (feature design section 6.3).
    """
    if isinstance(tool_use, dict):
        use_id = tool_use.get("id")
        name = tool_use["name"]
        tool_input = tool_use.get("input") or {}
    else:
        use_id = getattr(tool_use, "id", None)
        name = tool_use.name
        tool_input = getattr(tool_use, "input", None) or {}

    method = getattr(kit, name)
    try:
        result = method(**tool_input)
        return {
            "type": "tool_result",
            "tool_use_id": use_id,
            "content": json.dumps(result),
        }
    except CortrixError as exc:
        return {
            "type": "tool_result",
            "tool_use_id": use_id,
            "is_error": True,
            "content": json.dumps(_error_payload(exc)),
        }
