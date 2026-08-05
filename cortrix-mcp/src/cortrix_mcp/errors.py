"""Agent-visible tool errors for the Cortrix MCP adapter.

Tool, backend, auth, quota, timeout, and validation failures are application
outcomes. They must reach the model as ``CallToolResult(is_error=True)`` with
the GEN-Agent fields intact. ``MCPError`` is intentionally not used here; the
SDK reserves it for JSON-RPC and protocol failures handled by the MCP host.

Business errors from cortrix-server (CX_ERR_NS_*, CX_ERR_MEMORY_*,
CX_ERR_OPLOG_*, and similar codes) pass through unchanged.
"""

from __future__ import annotations

import json
from typing import Any, Optional

from mcp.types import CallToolResult, TextContent

# Six adapter error codes retained from the v1 contract. Although the code
# prefix contains "MCP", these are model-visible tool outcomes, not JSON-RPC
# protocol errors.
TOOL_ERROR_TABLE: dict[str, dict[str, Any]] = {
    "CX_ERR_MCP_BACKEND_TIMEOUT": {
        "retryable": True,
        "category": "transient",
        "retry_after_ms": 1000,
        "message": "Cortrix backend timeout",
    },
    "CX_ERR_MCP_BACKEND_UNAVAILABLE": {
        "retryable": True,
        "category": "transient",
        "retry_after_ms": 5000,
        "message": "Cortrix backend unavailable",
    },
    "CX_ERR_MCP_SCHEMA_VALIDATION_FAIL": {
        "retryable": False,
        "category": "permanent",
        "retry_after_ms": None,
        "message": "Tool input schema validation failed",
    },
    "CX_ERR_MCP_TOOL_NOT_FOUND": {
        "retryable": False,
        "category": "permanent",
        "retry_after_ms": None,
        "message": "Tool not found",
    },
    "CX_ERR_MCP_AUTH_MISSING": {
        "retryable": False,
        "category": "auth",
        "retry_after_ms": None,
        "message": "CORTRIX_API_KEY is not set but the backend requires authentication",
    },
    "CX_ERR_MCP_ADMIN_REQUIRED": {
        "retryable": False,
        "category": "auth",
        "retry_after_ms": None,
        "message": "Admin role is required to call this tool",
    },
}

CATEGORY_ENUM = ("auth", "quota", "transient", "permanent", "timeout", "success")


class CortrixToolError(Exception):
    """Internal carrier for a model-visible Cortrix tool failure."""

    def __init__(self, message: str, data: dict[str, Any]) -> None:
        self.message = message
        self.data = data
        super().__init__(f"{data['code']}: {message}")


def success_envelope(
    data: Any,
    structured_data: Optional[dict[str, Any]] = None,
) -> dict[str, Any]:
    """Wrap business data in the stable GEN-Agent success envelope."""
    return {
        "data": data,
        "meta": {
            "retryable": False,
            "category": "success",
            "retry_after_ms": None,
            "structured_data": structured_data or {},
        },
    }


def tool_error(
    code: str,
    *,
    message: Optional[str] = None,
    structured_data: Optional[dict[str, Any]] = None,
) -> CortrixToolError:
    """Build one of the six stable adapter tool errors."""
    entry = TOOL_ERROR_TABLE.get(code)
    if entry is None:
        raise KeyError(f"Unknown MCP tool error code: {code}")
    return CortrixToolError(
        message or entry["message"],
        {
            "code": code,
            "retryable": entry["retryable"],
            "category": entry["category"],
            "retry_after_ms": entry["retry_after_ms"],
            "structured_data": structured_data or {},
        },
    )


def internal_tool_error() -> CortrixToolError:
    """Return a secret-free fallback for an unexpected tool execution failure."""
    return CortrixToolError(
        "Tool execution failed",
        {
            "code": "CX_ERR_INTERNAL_ERROR",
            "retryable": False,
            "category": "permanent",
            "retry_after_ms": None,
            "structured_data": {},
        },
    )


def tool_error_result(error: CortrixToolError) -> CallToolResult:
    """Convert an internal tool failure into a model-visible MCP result.

    The text block repeats the structured fields so legacy model integrations
    that only render ``content`` retain the same retry and correlation data.
    """
    payload = dict(error.data)
    text = f"{error.message}\n{json.dumps(payload, sort_keys=True, separators=(',', ':'))}"
    return CallToolResult(
        content=[TextContent(type="text", text=text)],
        structured_content=payload,
        is_error=True,
    )


def passthrough_business_error(
    status_code: int,
    error_body: dict[str, Any],
    identity: Optional[dict[str, Any]] = None,
) -> CortrixToolError:
    """Pass through a cortrix-server business error without changing its code.

    Caller identity is merged into ``structured_data`` while backend-supplied
    keys retain precedence. ``status_code`` is used only for the fallback
    message and is not added to the public error envelope.
    """
    body = error_body or {}
    structured: dict[str, Any] = dict(identity or {})
    structured.update(body.get("structured_data") or {})
    return CortrixToolError(
        body.get("message", f"Backend error (HTTP {status_code})"),
        {
            "code": body.get("code", "CX_ERR_INTERNAL_ERROR"),
            "retryable": body.get("retryable", False),
            "category": body.get("category", "permanent"),
            "retry_after_ms": body.get("retry_after_ms"),
            "structured_data": structured,
        },
    )
