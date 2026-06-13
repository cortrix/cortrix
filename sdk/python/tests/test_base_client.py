"""S1 path-agnostic transport tests: headers, exception mapping, retry decision.

These exercise the shared BaseClient layer directly via a throwaway path
(``/_probe``) so they don't depend on the S2/S3 resource path decisions.
"""

from __future__ import annotations

import httpx
import pytest
import respx

from cortrix import (
    AuthenticationError,
    Cortrix,
    ConflictError,
    ConnectionError,
    ForbiddenError,
    FeatureNotAvailableError,
    InvalidRequestError,
    NamespaceNotFoundError,
    NotFoundError,
    PayloadTooLargeError,
    RateLimitError,
    ServiceUnavailableError,
    TimeoutError,
)

PROBE = "/_probe"


def _url(api_base: str) -> str:
    return api_base + PROBE


# --- headers ---


@respx.mock
def test_headers_auth_tenant_user_agent_request_id(api_base: str) -> None:
    route = respx.get(_url(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090", api_key="cx_live_abc", tenant_id="t-1")
    c._request("GET", PROBE)
    req = route.calls.last.request
    assert req.headers["Authorization"] == "Bearer cx_live_abc"
    assert req.headers["X-Tenant-Id"] == "t-1"
    assert req.headers["User-Agent"].startswith("cortrix-python/")
    assert req.headers["X-Request-ID"]
    c.close()


@respx.mock
def test_no_auth_header_when_no_api_key(api_base: str) -> None:
    route = respx.get(_url(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090")
    c._request("GET", PROBE)
    assert "Authorization" not in route.calls.last.request.headers
    c.close()


@respx.mock
def test_client_id_and_traceparent_injected(api_base: str) -> None:
    route = respx.get(_url(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(
        base_url="http://testserver:9090",
        client_id="my-rag-app",
        trace_id_provider=lambda: "00-11111111111111111111111111111111-2222222222222222-01",
    )
    c._request("GET", PROBE)
    req = route.calls.last.request
    assert req.headers["X-Client-Id"] == "my-rag-app"
    assert req.headers["traceparent"].startswith("00-")
    c.close()


@respx.mock
def test_no_traceparent_when_no_provider(api_base: str) -> None:
    route = respx.get(_url(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090")
    c._request("GET", PROBE)
    assert "traceparent" not in route.calls.last.request.headers
    c.close()


@respx.mock
def test_trace_provider_exception_swallowed(api_base: str) -> None:
    def boom() -> str:
        raise RuntimeError("provider down")

    route = respx.get(_url(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090", trace_id_provider=boom)
    c._request("GET", PROBE)  # must not raise
    assert "traceparent" not in route.calls.last.request.headers
    c.close()


# --- exception mapping ---


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
        (503, ServiceUnavailableError),
        (504, TimeoutError),
    ],
)
@respx.mock
def test_status_to_exception(api_base: str, status: int, exc: type) -> None:
    respx.get(_url(api_base)).mock(
        return_value=httpx.Response(
            status, json={"error": {"code": "CX_ERR_X", "message": "m", "retryable": False}}
        )
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(exc):
        c._request("GET", PROBE)
    c.close()


@respx.mock
def test_403_feature_maps_to_feature_not_available(api_base: str) -> None:
    respx.get(_url(api_base)).mock(
        return_value=httpx.Response(
            403,
            json={"error": {"code": "CX_ERR_FEATURE_NOT_AVAILABLE", "message": "ent only"}},
        )
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(FeatureNotAvailableError):
        c._request("GET", PROBE)
    c.close()


@respx.mock
def test_404_namespace_path_maps_to_namespace_not_found(api_base: str) -> None:
    respx.get(api_base + "/namespaces/missing").mock(
        return_value=httpx.Response(404, json={"error": {"code": "CX_ERR_NS", "message": "nope"}})
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(NamespaceNotFoundError):
        c._request("GET", "/namespaces/missing")
    c.close()


@respx.mock
def test_error_carries_4_genagent_fields(api_base: str) -> None:
    respx.get(_url(api_base)).mock(
        return_value=httpx.Response(
            400,
            json={
                "error": {
                    "code": "CX_ERR_BAD",
                    "message": "bad",
                    "retryable": False,
                    "category": "permanent",
                    "retry_after_ms": None,
                    "structured_data": {"field": "namespace"},
                }
            },
        )
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(InvalidRequestError) as ei:
        c._request("GET", PROBE)
    e = ei.value
    assert e.retryable is False
    assert e.category == "permanent"
    assert e.retry_after_ms is None
    assert e.structured_data == {"field": "namespace"}
    assert e.error_code == "CX_ERR_BAD"
    c.close()


# --- retry decision (issue 4 priority) ---


@respx.mock
def test_retry_when_server_says_true(api_base: str) -> None:
    route = respx.get(_url(api_base))
    route.side_effect = [
        httpx.Response(503, json={"error": {"code": "CX_ERR_DB", "message": "busy",
                                             "retryable": True, "retry_after_ms": 1}}),
        httpx.Response(200, json={"ok": True}),
    ]
    c = Cortrix(base_url="http://testserver:9090", max_retries=3)
    out = c._request("GET", PROBE)
    assert out == {"ok": True}
    assert route.call_count == 2
    c.close()


@respx.mock
def test_no_retry_when_server_says_false(api_base: str) -> None:
    route = respx.get(_url(api_base)).mock(
        return_value=httpx.Response(
            429, json={"error": {"code": "CX_ERR_QUOTA", "message": "out", "retryable": False}}
        )
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=3)
    with pytest.raises(RateLimitError) as ei:
        c._request("GET", PROBE)
    assert ei.value.retryable is False
    assert route.call_count == 1  # did NOT retry despite 429
    c.close()


@respx.mock
def test_fallback_http_status_retry_when_no_retryable_field(api_base: str) -> None:
    route = respx.get(_url(api_base))
    route.side_effect = [
        httpx.Response(503, json={"error": {"code": "CX_ERR_X", "message": "down"}}),
        httpx.Response(200, json={"ok": 1}),
    ]
    c = Cortrix(base_url="http://testserver:9090", max_retries=2)
    assert c._request("GET", PROBE) == {"ok": 1}
    assert route.call_count == 2
    c.close()


@respx.mock
def test_max_retries_exhausted_raises(api_base: str) -> None:
    route = respx.get(_url(api_base)).mock(
        return_value=httpx.Response(503, json={"error": {"code": "CX_ERR_X", "message": "down"}})
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=2)
    with pytest.raises(ServiceUnavailableError):
        c._request("GET", PROBE)
    assert route.call_count == 3  # initial + 2 retries
    c.close()


@respx.mock
def test_400_never_retries(api_base: str) -> None:
    route = respx.get(_url(api_base)).mock(
        return_value=httpx.Response(400, json={"error": {"code": "CX_ERR_BAD", "message": "x"}})
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=3)
    with pytest.raises(InvalidRequestError):
        c._request("GET", PROBE)
    assert route.call_count == 1
    c.close()


@respx.mock
def test_network_error_maps_to_connection_error(api_base: str) -> None:
    respx.get(_url(api_base)).mock(side_effect=httpx.ConnectError("refused"))
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(ConnectionError):
        c._request("GET", PROBE)
    c.close()
