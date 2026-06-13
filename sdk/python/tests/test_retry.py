"""Retry behaviour: backoff growth, Retry-After respect, max-retries, no-retry."""

from __future__ import annotations

import time

import httpx
import pytest
import respx

from cortrix import (
    Cortrix,
    InternalServerError,
    InvalidRequestError,
    ServiceUnavailableError,
)

PROBE = "/_probe"


def _u(api_base: str) -> str:
    return api_base + PROBE


@respx.mock
def test_retry_429_then_success(api_base: str) -> None:
    route = respx.get(_u(api_base))
    route.side_effect = [
        httpx.Response(429, headers={"Retry-After": "0"}, json={"error": {"code": "CX_ERR_RL", "message": "x"}}),
        httpx.Response(200, json={"ok": 1}),
    ]
    c = Cortrix(base_url="http://testserver:9090", max_retries=2)
    assert c._request("GET", PROBE) == {"ok": 1}
    assert route.call_count == 2
    c.close()


@respx.mock
def test_retry_respects_retry_after_seconds(api_base: str) -> None:
    route = respx.get(_u(api_base))
    route.side_effect = [
        httpx.Response(429, headers={"Retry-After": "1"}, json={"error": {"code": "CX_ERR_RL", "message": "x"}}),
        httpx.Response(200, json={"ok": 1}),
    ]
    c = Cortrix(base_url="http://testserver:9090", max_retries=2)
    start = time.monotonic()
    c._request("GET", PROBE)
    assert (time.monotonic() - start) >= 0.9  # waited ~1s
    c.close()


@respx.mock
def test_retry_500_exponential_backoff_grows(api_base: str) -> None:
    # 3 x 500 (no retryable field) -> fallback backoff; total wait >= 0.5 + 1.0
    route = respx.get(_u(api_base)).mock(
        return_value=httpx.Response(500, json={"error": {"code": "CX_ERR_X", "message": "x"}})
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=2)
    start = time.monotonic()
    with pytest.raises(InternalServerError):
        c._request("GET", PROBE)
    elapsed = time.monotonic() - start
    assert route.call_count == 3  # initial + 2 retries
    assert elapsed >= 1.4  # ~0.5 + ~1.0 backoff (plus jitter)
    c.close()


@respx.mock
def test_max_retries_exhausted(api_base: str) -> None:
    route = respx.get(_u(api_base)).mock(
        return_value=httpx.Response(503, headers={"Retry-After": "0"},
                                    json={"error": {"code": "CX_ERR_X", "message": "x"}})
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=2)
    with pytest.raises(ServiceUnavailableError):
        c._request("GET", PROBE)
    assert route.call_count == 3
    c.close()


@respx.mock
def test_400_never_retries(api_base: str) -> None:
    route = respx.get(_u(api_base)).mock(
        return_value=httpx.Response(400, json={"error": {"code": "CX_ERR_BAD", "message": "x"}})
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=3)
    with pytest.raises(InvalidRequestError):
        c._request("GET", PROBE)
    assert route.call_count == 1
    c.close()


@respx.mock
def test_max_retries_zero_disables_retry(api_base: str) -> None:
    route = respx.get(_u(api_base)).mock(
        return_value=httpx.Response(503, json={"error": {"code": "CX_ERR_X", "message": "x", "retryable": True}})
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(ServiceUnavailableError):
        c._request("GET", PROBE)
    assert route.call_count == 1  # no retry even though retryable=true
    c.close()
