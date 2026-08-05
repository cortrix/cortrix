"""agent error codes + GEN-Agent 4-field error response (design section 9.3 / section 10).

GEN-Agent first principle (AGENT_FRIENDLY.md): every error carries machine-readable
fields so an Agent caller can decide retry / routing autonomously. The wire shape is::

    {"error": {"code", "message", "retryable", "category",
               "retry_after_ms", "structured_data"}}

``category`` is an enumerable value: auth / quota / transient / permanent / timeout.

Scope (design section 10):
  * ERROR_TABLE          = the 7 V1.0-enabled runtime chat-path codes (section 10.1).
  * STARTUP_ERROR_TABLE  = the 5 startup/init-period codes (section 10.3); these are a
    separate group from the "10 locked" runtime codes.

V1.5 stub codes (section 10.2) and D3-implementation boundary codes (section 10.4) are
intentionally NOT emitted by the V1.0 ChatExecutor and are therefore not registered here.
"""

from __future__ import annotations

from typing import Any, Literal, Optional

ErrorCategory = Literal["auth", "quota", "transient", "permanent", "timeout"]


# --- Section 10.1: 7 V1.0-enabled runtime chat-path error codes ---
# Each entry: category, retryable, default HTTP status, default message.
ERROR_TABLE: dict[str, dict[str, Any]] = {
    "CX_ERR_F48_LLM_TIMEOUT": {
        "category": "timeout",
        "retryable": True,
        "http_status": 504,
        "message": "LLM provider request timed out",
    },
    "CX_ERR_F48_LLM_QUOTA_EXCEEDED": {
        "category": "quota",
        "retryable": True,
        "http_status": 429,
        "message": "LLM provider quota exceeded",
    },
    "CX_ERR_F48_LLM_UNAVAILABLE": {
        "category": "transient",
        "retryable": True,
        "http_status": 503,
        "message": "LLM provider is unavailable",
    },
    "CX_ERR_F48_LLM_INVALID_CONFIG": {
        "category": "permanent",
        "retryable": False,
        "http_status": 400,
        "message": "LLM provider configuration is invalid",
    },
    "CX_ERR_F48_SESSION_NOT_FOUND": {
        "category": "permanent",
        "retryable": False,
        "http_status": 404,
        "message": "Session not found",
    },
    "CX_ERR_F48_RAG_FAILED": {
        "category": "transient",
        "retryable": True,
        "http_status": 503,
        "message": "RAG retrieval against cortrix-server failed",
    },
    "CX_ERR_F48_INTERACTION_LOG_FAILED": {
        "category": "transient",
        "retryable": True,
        "http_status": 503,
        "message": "Writing the interaction log (F13) failed",
    },
}


# --- Section 10.3: startup / init-period error codes (independent group) ---
STARTUP_ERROR_TABLE: dict[str, dict[str, Any]] = {
    "CX_ERR_F48_CORTRIX_SERVER_UNREACHABLE": {
        "category": "transient",
        "retryable": True,
        "http_status": 503,
        "message": "cortrix-server health check failed (5 attempts)",
    },
    "CX_ERR_F48_LLM_PROVIDER_INVALID": {
        "category": "permanent",
        "retryable": False,
        "http_status": 500,
        "message": "LLM provider registration failed (bad config)",
    },
    "CX_ERR_F48_PORT_OCCUPIED": {
        "category": "permanent",
        "retryable": False,
        "http_status": 500,
        "message": "Agent port is already in use",
    },
    "CX_ERR_F48_TENANT_CONFIG_INVALID": {
        "category": "permanent",
        "retryable": False,
        "http_status": 500,
        "message": "IGlobalConfig agent_llm section schema is invalid",
    },
    "CX_ERR_F48_NO_LLM_AVAILABLE": {
        "category": "transient",
        "retryable": True,
        "http_status": 503,
        "message": "No LLM provider is currently available",
    },
}

# Combined lookup used when building a response from any known agent code.
_ALL_CODES: dict[str, dict[str, Any]] = {**ERROR_TABLE, **STARTUP_ERROR_TABLE}


class AgentError(Exception):
    """A Cortrix Agent error carrying the GEN-Agent 4 fields.

    When ``code`` is a known agent code, ``category`` / ``retryable`` / ``http_status``
    default from the registry; callers may override any of them and attach a
    ``structured_data`` payload (design section 9.3 — the data the Agent needs to act).
    """

    def __init__(
        self,
        code: str,
        *,
        message: Optional[str] = None,
        category: Optional[ErrorCategory] = None,
        retryable: Optional[bool] = None,
        retry_after_ms: Optional[int] = None,
        structured_data: Optional[dict[str, Any]] = None,
        http_status: Optional[int] = None,
    ) -> None:
        entry = _ALL_CODES.get(code, {})
        self.code = code
        self.message = message or entry.get("message", code)
        self.category: ErrorCategory = category or entry.get("category", "permanent")
        self.retryable = retryable if retryable is not None else entry.get("retryable", False)
        self.retry_after_ms = retry_after_ms
        self.structured_data = structured_data or {}
        self.http_status = http_status or entry.get("http_status", 500)
        super().__init__(self.message)

    def to_dict(self) -> dict[str, Any]:
        """Return the GEN-Agent 4-field error envelope (design section 9.3)."""
        return {
            "error": {
                "code": self.code,
                "message": self.message,
                "retryable": self.retryable,
                "category": self.category,
                "retry_after_ms": self.retry_after_ms,
                "structured_data": self.structured_data,
            }
        }


def agent_error_response(
    code: str,
    *,
    message: Optional[str] = None,
    category: Optional[ErrorCategory] = None,
    retryable: Optional[bool] = None,
    retry_after_ms: Optional[int] = None,
    structured_data: Optional[dict[str, Any]] = None,
) -> dict[str, Any]:
    """Build the GEN-Agent 4-field error envelope for ``code`` without raising.

    Convenience wrapper around :class:`AgentError` for SSE / JSON responses.
    """
    return AgentError(
        code,
        message=message,
        category=category,
        retryable=retryable,
        retry_after_ms=retry_after_ms,
        structured_data=structured_data,
    ).to_dict()
