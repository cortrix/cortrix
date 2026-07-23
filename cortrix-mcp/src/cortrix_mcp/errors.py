"""GEN-Agent two-layer error model + the 6 CX_ERR_MCP_* protocol error codes.

Two-layer contract (feature design section 6):
  * MCP protocol layer: raise ``McpError`` (isError=True) so the IDE tool bar shows red.
  * Body JSON layer: the 4 GEN-Agent fields (retryable / category / retry_after_ms /
    structured_data) are carried inside ``ErrorData.data`` so programmatic agents can
    decide retry behavior.

Business errors from cortrix-server (CX_ERR_NS_*, CX_ERR_MEM03_*, CX_ERR_OPLOG_* ...)
are passed through unchanged — this module never re-invents business codes.
"""

from __future__ import annotations

from typing import Any, Optional

from mcp import ErrorData, McpError
from mcp.types import (
    INTERNAL_ERROR,
    INVALID_PARAMS,
    INVALID_REQUEST,
    METHOD_NOT_FOUND,
)

# ---------------------------------------------------------------------------
# 6 CX_ERR_MCP_* protocol error codes (feature design section 7.1 / brief section 3)
# Each entry: jsonrpc_code, retryable, category, retry_after_ms, default message.
# ---------------------------------------------------------------------------
MCP_ERROR_TABLE: dict[str, dict[str, Any]] = {
    "CX_ERR_MCP_BACKEND_TIMEOUT": {
        "jsonrpc": INTERNAL_ERROR,
        "retryable": True,
        "category": "transient",
        "retry_after_ms": 1000,
        "message": "Cortrix backend timeout",
    },
    "CX_ERR_MCP_BACKEND_UNAVAILABLE": {
        "jsonrpc": INTERNAL_ERROR,
        "retryable": True,
        "category": "transient",
        "retry_after_ms": 5000,
        "message": "Cortrix backend unavailable",
    },
    "CX_ERR_MCP_SCHEMA_VALIDATION_FAIL": {
        "jsonrpc": INVALID_PARAMS,
        "retryable": False,
        "category": "permanent",
        "retry_after_ms": None,
        "message": "Tool input schema validation failed",
    },
    "CX_ERR_MCP_TOOL_NOT_FOUND": {
        "jsonrpc": METHOD_NOT_FOUND,
        "retryable": False,
        "category": "permanent",
        "retry_after_ms": None,
        "message": "Tool not found",
    },
    "CX_ERR_MCP_AUTH_MISSING": {
        "jsonrpc": INVALID_REQUEST,
        "retryable": False,
        "category": "auth",
        "retry_after_ms": None,
        "message": "CORTRIX_API_KEY is not set but the backend requires authentication",
    },
    "CX_ERR_MCP_ADMIN_REQUIRED": {
        "jsonrpc": INVALID_REQUEST,
        "retryable": False,
        "category": "auth",
        "retry_after_ms": None,
        "message": "Admin role is required to call this tool",
    },
}

# Allowed GEN-Agent category enum (AGENT_FRIENDLY principle 4).
CATEGORY_ENUM = ("auth", "quota", "transient", "permanent", "timeout", "success")


def success_envelope(
    data: Any,
    structured_data: Optional[dict[str, Any]] = None,
) -> dict[str, Any]:
    """Wrap business ``data`` in the GEN-Agent two-layer success envelope.

    Returns ``{"data": ..., "meta": {retryable, category, retry_after_ms, structured_data}}``.
    """
    return {
        "data": data,
        "meta": {
            "retryable": False,
            "category": "success",
            "retry_after_ms": None,
            "structured_data": structured_data or {},
        },
    }


def mcp_error(
    code: str,
    *,
    message: Optional[str] = None,
    structured_data: Optional[dict[str, Any]] = None,
) -> McpError:
    """Build an ``McpError`` for one of the 6 CX_ERR_MCP_* protocol codes.

    The 4 GEN-Agent fields are sourced from ``MCP_ERROR_TABLE`` so the body JSON layer
    stays consistent with feature design section 7.1.
    """
    entry = MCP_ERROR_TABLE.get(code)
    if entry is None:  # pragma: no cover - guards against typos in callers
        raise KeyError(f"Unknown MCP error code: {code}")
    return McpError(
        ErrorData(
            code=entry["jsonrpc"],
            message=message or entry["message"],
            data={
                "code": code,
                "retryable": entry["retryable"],
                "category": entry["category"],
                "retry_after_ms": entry["retry_after_ms"],
                "structured_data": structured_data or {},
            },
        )
    )


# Map an upstream HTTP status to the closest JSON-RPC protocol code for pass-through.
_HTTP_TO_JSONRPC = {
    400: INVALID_PARAMS,
    401: INVALID_REQUEST,
    403: INVALID_REQUEST,
    404: INVALID_REQUEST,
    429: INTERNAL_ERROR,
    500: INTERNAL_ERROR,
    503: INTERNAL_ERROR,
    504: INTERNAL_ERROR,
}


def passthrough_business_error(status_code: int, error_body: dict[str, Any]) -> McpError:
    """Pass through a cortrix-server business error (CX_ERR_*) unchanged.

    The backend already emits the GEN-Agent 4 fields (AGENT_FRIENDLY principle 1); this
    function only re-wraps them into ``McpError`` for the MCP protocol layer. Business
    codes are never re-invented (feature design section 7.2).
    """
    body = error_body or {}
    return McpError(
        ErrorData(
            code=_HTTP_TO_JSONRPC.get(status_code, INTERNAL_ERROR),
            message=body.get("message", f"Backend error (HTTP {status_code})"),
            data={
                "code": body.get("code", "CX_ERR_INTERNAL_ERROR"),
                "retryable": body.get("retryable", False),
                "category": body.get("category", "permanent"),
                "retry_after_ms": body.get("retry_after_ms"),
                "structured_data": body.get("structured_data", {}),
            },
        )
    )
