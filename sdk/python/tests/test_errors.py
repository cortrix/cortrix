"""Error mapping tests: L1 by status, L2 by code, hints, 4 fields."""

from __future__ import annotations

import httpx
import pytest
import respx

from cortrix import (
    AuthInvalidApiKeyError,
    AuthInvalidCredentialsError,
    AuthTokenExpiredError,
    AuthenticationError,
    ConflictError,
    Cortrix,
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
    StoreDbError,
    StoreNotFoundError,
)

PROBE = "/_probe"


def _err(code: str, **extra) -> dict:
    return {"error": {"code": code, "message": "m", **extra}}


@pytest.mark.parametrize(
    "status,exc",
    [
        (400, InvalidRequestError),
        (401, AuthenticationError),
        (403, ForbiddenError),
        (404, NotFoundError),
        (409, ConflictError),
        (413, PayloadTooLargeError),
        (429, RateLimitError),
        (500, InternalServerError),
        (503, ServiceUnavailableError),
    ],
)
@respx.mock
def test_l1_status_mapping(api_base: str, status: int, exc: type) -> None:
    respx.get(api_base + PROBE).mock(
        return_value=httpx.Response(status, json=_err("CX_ERR_GENERIC", retryable=False))
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(exc):
        c._request("GET", PROBE)
    c.close()


@pytest.mark.parametrize(
    "code,exc",
    [
        ("CX_ERR_AUTH_INVALID_CREDENTIALS", AuthInvalidCredentialsError),
        ("CX_ERR_AUTH_TOKEN_EXPIRED", AuthTokenExpiredError),
        ("CX_ERR_AUTH_INVALID_API_KEY", AuthInvalidApiKeyError),
        ("CX_ERR_STORE_NOT_FOUND", StoreNotFoundError),
        ("CX_ERR_STORE_DB_ERROR", StoreDbError),
        ("CX_ERR_NAMESPACE_NOT_FOUND", NamespaceNotFoundError),
    ],
)
@respx.mock
def test_l2_code_mapping(api_base: str, code: str, exc: type) -> None:
    respx.get(api_base + PROBE).mock(
        return_value=httpx.Response(401, json=_err(code, retryable=False))
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(exc) as ei:
        c._request("GET", PROBE)
    assert ei.value.error_code == code
    # L2 still inherits its L1 base + CortrixError
    assert isinstance(ei.value, CortrixError)
    c.close()


@respx.mock
def test_403_feature_hint(api_base: str) -> None:
    respx.get(api_base + PROBE).mock(
        return_value=httpx.Response(403, json=_err("CX_ERR_FEATURE_NOT_AVAILABLE"))
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(FeatureNotAvailableError):
        c._request("GET", PROBE)
    c.close()


@respx.mock
def test_404_namespace_path_hint(api_base: str) -> None:
    respx.get(api_base + "/namespaces/x").mock(
        return_value=httpx.Response(404, json=_err("CX_ERR_NOT_FOUND"))
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(NamespaceNotFoundError):
        c._request("GET", "/namespaces/x")
    c.close()


@respx.mock
def test_quota_prefix_maps_to_quota_exceeded(api_base: str) -> None:
    respx.get(api_base + PROBE).mock(
        return_value=httpx.Response(429, json=_err("CX_ERR_QUOTA_TENANT_EXCEEDED", retryable=False))
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(QuotaExceededError):
        c._request("GET", PROBE)
    c.close()


@respx.mock
def test_non_json_error_body_preserved(api_base: str) -> None:
    respx.get(api_base + PROBE).mock(
        return_value=httpx.Response(500, text="<html>boom</html>")
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(InternalServerError) as ei:
        c._request("GET", PROBE)
    assert ei.value.body is not None
    c.close()


@respx.mock
def test_request_id_extracted_from_body(api_base: str) -> None:
    respx.get(api_base + PROBE).mock(
        return_value=httpx.Response(400, json=_err("CX_ERR_BAD", request_id="req_xyz"))
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(InvalidRequestError) as ei:
        c._request("GET", PROBE)
    assert ei.value.request_id == "req_xyz"
    c.close()
