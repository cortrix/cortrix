"""SDK agent cases 1-5 — GEN-Agent 4-field decisions."""

from __future__ import annotations

import time

import httpx
import pytest
import respx

from cortrix import Cortrix, InvalidRequestError, RateLimitError

PROBE = "/_probe"


def _u(api_base: str) -> str:
    return api_base + PROBE


@respx.mock
def test_agent_1_error_has_4_fields(api_base: str) -> None:
    """SDK agent case 1: 400 carries retryable/category/retry_after_ms/structured_data."""
    respx.get(_u(api_base)).mock(
        return_value=httpx.Response(
            400,
            json={"error": {"code": "CX_ERR_BAD", "message": "bad", "retryable": False,
                            "category": "permanent", "retry_after_ms": None,
                            "structured_data": {"field": "x"}}},
        )
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=0)
    with pytest.raises(InvalidRequestError) as ei:
        c._request("GET", PROBE)
    e = ei.value
    assert e.retryable is False and e.category == "permanent"
    assert e.retry_after_ms is None and e.structured_data == {"field": "x"}
    c.close()


@respx.mock
def test_agent_2_retry_when_server_says_true(api_base: str) -> None:
    """SDK agent case 2: 503 retryable=true + retry_after_ms, succeeds on 3rd."""
    route = respx.get(_u(api_base))
    route.side_effect = [
        httpx.Response(503, json={"error": {"code": "CX_ERR_DB", "message": "x",
                                             "retryable": True, "retry_after_ms": 10}}),
        httpx.Response(503, json={"error": {"code": "CX_ERR_DB", "message": "x",
                                             "retryable": True, "retry_after_ms": 10}}),
        httpx.Response(200, json={"ok": True}),
    ]
    c = Cortrix(base_url="http://testserver:9090", max_retries=3)
    assert c._request("GET", PROBE) == {"ok": True}
    assert route.call_count == 3
    c.close()


@respx.mock
def test_agent_3_no_retry_when_server_says_false(api_base: str) -> None:
    """SDK agent case 3: 429 retryable=false (quota) -> no retry, RateLimitError."""
    route = respx.get(_u(api_base)).mock(
        return_value=httpx.Response(
            429, json={"error": {"code": "CX_ERR_QUOTA", "message": "out",
                                 "retryable": False, "category": "quota"}}
        )
    )
    c = Cortrix(base_url="http://testserver:9090", max_retries=3)
    with pytest.raises(RateLimitError) as ei:
        c._request("GET", PROBE)
    assert ei.value.retryable is False and ei.value.category == "quota"
    assert route.call_count == 1
    c.close()


@respx.mock
def test_agent_4_fallback_http_status(api_base: str) -> None:
    """SDK agent case 4: 503 with NO retryable field -> fallback HTTP-status retry."""
    route = respx.get(_u(api_base))
    route.side_effect = [
        httpx.Response(503, json={"error": {"code": "CX_ERR_X", "message": "down"}}),
        httpx.Response(200, json={"ok": 1}),
    ]
    c = Cortrix(base_url="http://testserver:9090", max_retries=2)
    assert c._request("GET", PROBE) == {"ok": 1}
    assert route.call_count == 2
    c.close()


@respx.mock
def test_agent_5_retry_after_ms_priority(api_base: str) -> None:
    """SDK agent case 5: retry_after_ms=120 wins over Retry-After header '5' (s)."""
    route = respx.get(_u(api_base))
    route.side_effect = [
        httpx.Response(
            503,
            headers={"Retry-After": "5"},
            json={"error": {"code": "CX_ERR_X", "message": "x",
                            "retryable": True, "retry_after_ms": 120}},
        ),
        httpx.Response(200, json={"ok": 1}),
    ]
    c = Cortrix(base_url="http://testserver:9090", max_retries=2)
    start = time.monotonic()
    assert c._request("GET", PROBE) == {"ok": 1}
    elapsed = time.monotonic() - start
    # waited ~0.12s (retry_after_ms), NOT ~5s (header)
    assert 0.1 <= elapsed < 1.0
    c.close()
