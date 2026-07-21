"""Shared client logic: headers, exception construction, retry decisions.

``BaseClient`` holds connection config and the pure (I/O-free) helpers shared by
the sync (:class:`cortrix._client.Cortrix`) and async
(:class:`cortrix._async_client.AsyncCortrix`) clients:

  - ``_build_headers`` — auth + User-Agent + X-Request-ID, plus issue-3
    ``client_id`` / ``trace_id_provider`` injection (§ 4.3.1).
  - ``_build_exception`` — parse the GEN-Agent 4 fields and pick the exception
    class (§ 4.3.2).
  - ``should_retry`` — retry decision with the issue-4 priority order (§ 5).

The actual HTTP send loops live on the sync/async subclasses (they own the
``httpx`` client); they call back into these helpers so behavior is identical
across both.
"""

from __future__ import annotations

import random
import uuid
from typing import Any, Callable, Optional, Type, Union

import httpx

from ._constants import (
    API_PREFIX,
    DEFAULT_BASE_URL,
    DEFAULT_MAX_RETRIES,
    DEFAULT_TIMEOUT,
    USER_AGENT,
)
from ._exceptions import (
    CODE_EXCEPTION_MAP,
    AuthenticationError,
    ConflictError,
    CortrixError,
    FeatureNotAvailableError,
    ForbiddenError,
    InternalServerError,
    InvalidRequestError,
    NamespaceNotFoundError,
    NotFoundError,
    PayloadTooLargeError,
    QuotaExceededError,
    RateLimitError,
    ServiceUnavailableError,
    TimeoutError,
)

# HTTP status -> L1 exception class. (L2 subclass refinement is driven by the
# server's ``error.code`` via the registry in _exceptions; the SDK selects the
# L1 base from status + a couple of path/code hints, per design § 4.3.2.)
_STATUS_EXCEPTION_MAP: dict[int, Type[CortrixError]] = {
    400: InvalidRequestError,
    401: AuthenticationError,
    403: ForbiddenError,
    404: NotFoundError,
    409: ConflictError,
    413: PayloadTooLargeError,
    422: InvalidRequestError,
    429: RateLimitError,
    500: InternalServerError,
    503: ServiceUnavailableError,
    504: TimeoutError,
}

# Status codes eligible for fallback retry when the server omits ``retryable``.
_FALLBACK_RETRY_STATUS = frozenset({429, 500, 503})

_MAX_BACKOFF = 8.0
_BASE_BACKOFF = 0.5


def _parse_retry_after(value: str) -> float:
    """Parse a Retry-After header value (seconds form only; RFC 7231 date form
    is uncommon for this API and falls back to a small default)."""
    try:
        seconds = float(value)
    except (TypeError, ValueError):
        return _BASE_BACKOFF
    return max(0.0, seconds)


def _exponential_backoff(attempt: int) -> float:
    """min(0.5 * 2^attempt + jitter, 8.0); jitter = U(0, 0.25 * base)."""
    base: float = _BASE_BACKOFF * (2 ** attempt)
    jitter: float = random.uniform(0.0, 0.25 * base)
    return min(base + jitter, _MAX_BACKOFF)


class BaseClient:
    """Connection config + shared, I/O-free request helpers."""

    def __init__(
        self,
        *,
        base_url: str = DEFAULT_BASE_URL,
        api_key: Optional[str] = None,
        tenant_id: Optional[str] = None,
        timeout: float = DEFAULT_TIMEOUT,
        max_retries: int = DEFAULT_MAX_RETRIES,
        client_id: Optional[str] = None,
        trace_id_provider: Optional[Callable[[], str]] = None,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.tenant_id = tenant_id
        self.timeout = timeout
        self.max_retries = max_retries
        self.client_id = client_id
        self.trace_id_provider = trace_id_provider

    # --- URL ---

    def _url(self, path: str) -> str:
        """Join base_url + API prefix + path. ``path`` is a server-relative
        path *without* the /api/v1 prefix (the prefix is owned by the SDK)."""
        if not path.startswith("/"):
            path = "/" + path
        return f"{self.base_url}{API_PREFIX}{path}"

    # --- headers (§ 4.3.1, issue 3) ---

    def _build_headers(self, extra: Optional[dict[str, str]] = None) -> dict[str, str]:
        headers: dict[str, str] = {
            "User-Agent": USER_AGENT,
            "X-Request-ID": str(uuid.uuid4()),
        }
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        if self.tenant_id:
            headers["X-Tenant-Id"] = self.tenant_id

        # issue 3 — client_id + trace_id_provider injection.
        if self.client_id:
            headers["X-Client-Id"] = self.client_id
        if self.trace_id_provider is not None:
            try:
                traceparent = self.trace_id_provider()
                if traceparent:
                    headers["traceparent"] = traceparent
            except Exception:
                # A failing provider must never break the HTTP request
                # (defensive — design § 4.3.1).
                pass

        if extra:
            headers.update(extra)
        return headers

    # --- exception construction (§ 4.3.2, issue 4) ---

    def _select_exception_class(
        self, status_code: int, error_code: Optional[str], path: str
    ) -> Type[CortrixError]:
        # 1. Precise L2 subclass by error code (ERROR_CODE_SDK_MAP § 3).
        if error_code:
            mapped = CODE_EXCEPTION_MAP.get(error_code)
            if mapped is not None:
                return mapped
            if error_code.startswith("CX_ERR_QUOTA"):
                return QuotaExceededError
            # 2. code-based hints (design § 4.3.2).
            if status_code == 403 and "feature" in error_code.lower():
                return FeatureNotAvailableError
        # 3. path hint: namespace 404.
        if status_code == 404 and "namespace" in (path or "").lower():
            return NamespaceNotFoundError
        # 4. fallback: HTTP status -> L1 base.
        return _STATUS_EXCEPTION_MAP.get(status_code, CortrixError)

    def _build_exception(self, resp: httpx.Response) -> CortrixError:
        try:
            body = resp.json()
        except Exception:
            body = {"error": {"message": resp.text or "<no body>"}}
        if not isinstance(body, dict):
            body = {"error": {"message": str(body)}}

        err = body.get("error", {}) or {}
        path = resp.request.url.path if resp.request is not None else ""
        exc_class = self._select_exception_class(resp.status_code, err.get("code"), path)

        kwargs: dict[str, Any] = dict(
            status_code=resp.status_code,
            error_code=err.get("code"),
            request_id=resp.headers.get("X-Request-ID")
            or resp.headers.get("x-cortrix-trace-id")
            or err.get("request_id"),
            body=body,
            retryable=err.get("retryable"),
            category=err.get("category"),
            retry_after_ms=err.get("retry_after_ms"),
            structured_data=err.get("structured_data"),
        )
        if issubclass(exc_class, RateLimitError):
            ra = resp.headers.get("Retry-After")
            kwargs["retry_after"] = _parse_retry_after(ra) if ra is not None else None

        return exc_class(err.get("message", "Unknown error"), **kwargs)

    # --- retry decision (§ 5, issue 4) ---

    def should_retry(
        self,
        resp_or_exc: Union[httpx.Response, Exception],
        attempt: int,
    ) -> tuple[bool, float]:
        """Return ``(retry?, wait_seconds)``.

        Priority (design § 5):
          1. server ``retryable`` field — strict
          2. fallback HTTP status (429/500/503)
          3. network errors (ConnectError / TimeoutException) — retry
        Interval priority: ``retry_after_ms`` > Retry-After header > backoff.
        """
        if attempt >= self.max_retries:
            return (False, 0.0)

        # Network-layer errors → retry with backoff.
        if isinstance(resp_or_exc, (httpx.ConnectError, httpx.TimeoutException)):
            return (True, _exponential_backoff(attempt))
        if isinstance(resp_or_exc, Exception):
            return (False, 0.0)

        resp = resp_or_exc
        try:
            body = resp.json()
            err = body.get("error", {}) if isinstance(body, dict) else {}
        except Exception:
            err = {}

        # 1. GEN-Agent priority — server-supplied retryable.
        if isinstance(err, dict) and "retryable" in err:
            if not err["retryable"]:
                return (False, 0.0)
            rm = err.get("retry_after_ms")
            if rm is not None:
                return (True, rm / 1000.0)
            ra = resp.headers.get("Retry-After")
            if ra is not None:
                return (True, _parse_retry_after(ra))
            return (True, _exponential_backoff(attempt))

        # 2. fallback — HTTP status code.
        if resp.status_code in _FALLBACK_RETRY_STATUS:
            if resp.status_code == 429:
                ra = resp.headers.get("Retry-After", "1")
                return (True, _parse_retry_after(ra))
            return (True, _exponential_backoff(attempt))

        return (False, 0.0)
