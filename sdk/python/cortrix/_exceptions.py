"""Cortrix SDK exception hierarchy.

Layered mapping per ERROR_CODE_SDK_MAP.md:
  - L1 base classes (12): HTTP-status grouping.
  - L2 high-frequency subclasses (23 concrete here; SoT target 26 incl. extension
    slots — see ERROR_CODE_SDK_MAP and Python SDK design v1.0.3 tree note).
  - L3 default (100+): routed via L1 base + ``error_code`` / ``category`` metadata.

Every exception inherits ``CortrixError``, which carries the 4 GEN-Agent fields
(``retryable`` / ``category`` / ``retry_after_ms`` / ``structured_data``) so Agent
frameworks can make autonomous retry / routing decisions (the Agent-friendly contract,
issue 4).
"""

from __future__ import annotations

from typing import Any, Literal, Optional

# GEN-Agent first principle (the Agent-friendly contract) — enumerable category values.
ErrorCategory = Literal["auth", "quota", "transient", "permanent", "timeout"]


class CortrixError(Exception):
    """Base class for all Cortrix SDK exceptions.

    Carries the GEN-Agent 4 fields (``retryable`` / ``category`` /
    ``retry_after_ms`` / ``structured_data``) populated from the server error
    response when present (``None`` when absent — backward compatible with older
    servers).
    """

    def __init__(
        self,
        message: str,
        *,
        status_code: Optional[int] = None,
        error_code: Optional[str] = None,
        request_id: Optional[str] = None,
        body: Optional[dict[str, Any]] = None,
        # GEN-Agent 4 fields (issue 4)
        retryable: Optional[bool] = None,
        category: Optional[ErrorCategory] = None,
        retry_after_ms: Optional[int] = None,
        structured_data: Optional[dict[str, Any]] = None,
    ) -> None:
        super().__init__(message)
        self.message = message
        self.status_code = status_code
        self.error_code = error_code
        self.request_id = request_id
        self.body = body
        self.retryable = retryable
        self.category = category
        self.retry_after_ms = retry_after_ms
        self.structured_data = structured_data


# === L1 base classes (12) ===


class InvalidRequestError(CortrixError):
    """400 — malformed client request."""


class AuthenticationError(CortrixError):
    """401 — not authenticated or token expired."""


class ForbiddenError(CortrixError):
    """403 — lacks permission for the resource."""


class NotFoundError(CortrixError):
    """404 — resource does not exist."""


class ConflictError(CortrixError):
    """409 — resource conflict (e.g. duplicate namespace name)."""


class PayloadTooLargeError(CortrixError):
    """413 — request body / file too large."""


class RateLimitError(CortrixError):
    """429 — request rate exceeded."""

    def __init__(self, message: str, *, retry_after: Optional[float] = None, **kwargs: Any) -> None:
        super().__init__(message, **kwargs)
        # Convenience: seconds form of the Retry-After header (in addition to
        # the GEN-Agent ``retry_after_ms`` on the base class).
        self.retry_after = retry_after


class InternalServerError(CortrixError):
    """500 — server-side error."""


class ServiceUnavailableError(CortrixError):
    """503 — service temporarily unavailable."""


class FeatureNotAvailableError(CortrixError):
    """Feature not available on this deployment."""


class ConnectionError(CortrixError):
    """Network connection failure (DNS / connection refused)."""


class TimeoutError(CortrixError):
    """Request timed out (504 / client-side timeout)."""


# === L2 subclasses (23 concrete — ERROR_CODE_SDK_MAP / Python SDK tree) ===

# --- Namespace (1) ---
class NamespaceNotFoundError(NotFoundError):
    """404 — CX_ERR_NAMESPACE_NOT_FOUND — namespace does not exist."""


# --- Auth (11 — V3 decision 14, CX_ERR_AUTH_*) -------
class AuthEmailAlreadyExistsError(ConflictError):
    """409 — CX_ERR_AUTH_EMAIL_ALREADY_EXISTS — email already registered."""


class AuthInvalidCredentialsError(AuthenticationError):
    """401 — CX_ERR_AUTH_INVALID_CREDENTIALS — email/password mismatch."""


class AuthInvalidResetCodeError(InvalidRequestError):
    """400 — CX_ERR_AUTH_INVALID_RESET_CODE — reset code invalid/expired."""


class AuthTokenExpiredError(AuthenticationError):
    """401 — CX_ERR_AUTH_TOKEN_EXPIRED — JWT / API Key token expired."""


class AuthBootstrapTokenInvalidError(AuthenticationError):
    """401 — CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID — bootstrap token invalid/consumed."""


class AuthInvalidApiKeyError(AuthenticationError):
    """401 — CX_ERR_AUTH_INVALID_API_KEY — API Key invalid/revoked."""


class AuthAdminRequiredError(ForbiddenError):
    """403 — CX_ERR_AUTH_ADMIN_REQUIRED — admin role required."""


class AuthTokenVerificationFailedError(InternalServerError):
    """500 — CX_ERR_AUTH_TOKEN_VERIFICATION_FAILED — token signature verification failed."""


class AuthEmailSendFailedError(ServiceUnavailableError):
    """503 — CX_ERR_AUTH_EMAIL_SEND_FAILED — SMTP send failed."""


class AuthBcryptTimeoutError(InternalServerError):
    """500 — CX_ERR_AUTH_BCRYPT_TIMEOUT — bcrypt hashing timed out (V4 CAT-2 #14)."""


class AuthJwtInitFailedError(InternalServerError):
    """500 — CX_ERR_AUTH_JWT_INIT_FAILED — JWT module init failed."""


# --- cortrix::store (2 — V3 decision 2) ---
class StoreNotFoundError(NotFoundError):
    """404 — CX_ERR_STORE_NOT_FOUND — store key missing (e.g. ParentChunk parent_id)."""


class StoreDbError(ServiceUnavailableError):
    """503 — CX_ERR_STORE_DB_ERROR — underlying DB error (retryable)."""


# --- pgcortrix (1 — V3 decision 4) -------
class PgcortrixInvalidFilterError(InvalidRequestError):
    """400 — CX_ERR_PGCORTRIX_INVALID_FILTER — filter JSONB field not in allowlist."""


# --- Retrieval path (8 — RAG-Fusion/CRAG/doc summary/agent/memory extraction + LLM/quota/CSRF) ---
class RagFusionExpandTimeoutError(TimeoutError):
    """timeout — CX_ERR_RAGFUSION_EXPAND_TIMEOUT — RAG-Fusion ExpandQueries LLM timeout."""


class CragEvaluationFailedError(InternalServerError):
    """500 — CX_ERR_CRAG_EVAL_FAILED — CRAG LLM evaluation failed (fallback active)."""


class DocSummaryFailedError(InternalServerError):
    """500 — CX_ERR_DOCSUMMARY_FAILED — DocSummary LLM call failed."""


class AgentToolNotFoundError(NotFoundError):
    """404 — CX_ERR_AGENT_TOOL_NOT_FOUND — Cortrix Agent tool not registered."""


class MemoryExtractionFailedError(InternalServerError):
    """500 — CX_ERR_MEMEXTRACT_FAILED — memory extraction LLM extraction failed (fallback active)."""


class LlmCircuitOpenError(ServiceUnavailableError):
    """503 — CX_ERR_LLM_CIRCUIT_OPEN — global LLM circuit breaker open (V4 CC-08)."""


class QuotaExceededError(RateLimitError):
    """429 — CX_ERR_QUOTA_* — per-tenant quota exceeded (Cloud V1.5+)."""


class CsrfMismatchError(ForbiddenError):
    """403 — CX_ERR_AUTH_CSRF_MISMATCH — CSRF token missing/mismatched (V4 CC-02)."""


# Error-code -> L2 subclass registry (ERROR_CODE_SDK_MAP). The base client
# consults this first (high-frequency business codes get their precise subclass
# so Agents can ``isinstance``-route); unknown codes fall back to the HTTP-status
# L1 base. ``CX_ERR_QUOTA_*`` is handled by prefix in the selector, not here.
CODE_EXCEPTION_MAP: dict[str, type[CortrixError]] = {
    # Namespace
    "CX_ERR_NAMESPACE_NOT_FOUND": NamespaceNotFoundError,
    # Auth (11)
    "CX_ERR_AUTH_EMAIL_ALREADY_EXISTS": AuthEmailAlreadyExistsError,
    "CX_ERR_AUTH_INVALID_CREDENTIALS": AuthInvalidCredentialsError,
    "CX_ERR_AUTH_INVALID_RESET_CODE": AuthInvalidResetCodeError,
    "CX_ERR_AUTH_TOKEN_EXPIRED": AuthTokenExpiredError,
    "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID": AuthBootstrapTokenInvalidError,
    "CX_ERR_AUTH_INVALID_API_KEY": AuthInvalidApiKeyError,
    "CX_ERR_AUTH_ADMIN_REQUIRED": AuthAdminRequiredError,
    "CX_ERR_AUTH_TOKEN_VERIFICATION_FAILED": AuthTokenVerificationFailedError,
    "CX_ERR_AUTH_EMAIL_SEND_FAILED": AuthEmailSendFailedError,
    "CX_ERR_AUTH_BCRYPT_TIMEOUT": AuthBcryptTimeoutError,
    "CX_ERR_AUTH_JWT_INIT_FAILED": AuthJwtInitFailedError,
    "CX_ERR_AUTH_CSRF_MISMATCH": CsrfMismatchError,
    # Store (2)
    "CX_ERR_STORE_NOT_FOUND": StoreNotFoundError,
    "CX_ERR_STORE_DB_ERROR": StoreDbError,
    # Pgcortrix (1)
    "CX_ERR_PGCORTRIX_INVALID_FILTER": PgcortrixInvalidFilterError,
    # Retrieval path (6)
    "CX_ERR_RAGFUSION_EXPAND_TIMEOUT": RagFusionExpandTimeoutError,
    "CX_ERR_CRAG_EVAL_FAILED": CragEvaluationFailedError,
    "CX_ERR_DOCSUMMARY_FAILED": DocSummaryFailedError,
    "CX_ERR_AGENT_TOOL_NOT_FOUND": AgentToolNotFoundError,
    "CX_ERR_MEMEXTRACT_FAILED": MemoryExtractionFailedError,
    "CX_ERR_LLM_CIRCUIT_OPEN": LlmCircuitOpenError,
}
