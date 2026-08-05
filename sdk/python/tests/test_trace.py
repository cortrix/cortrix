"""SDK trace cases 1-4 — issue-3 client_id / trace_id_provider injection."""

from __future__ import annotations

import httpx
import respx

from cortrix import AsyncCortrix, Cortrix

PROBE = "/_probe"


def _u(api_base: str) -> str:
    return api_base + PROBE


@respx.mock
def test_trace_1_client_id_header(api_base: str) -> None:
    route = respx.get(_u(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090", client_id="my-rag-app")
    c._request("GET", PROBE)
    assert route.calls.last.request.headers["X-Client-Id"] == "my-rag-app"
    c.close()


@respx.mock
def test_trace_2_traceparent_inject(api_base: str) -> None:
    tp = "00-11111111111111111111111111111111-2222222222222222-01"
    route = respx.get(_u(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090", trace_id_provider=lambda: tp)
    c._request("GET", PROBE)
    assert route.calls.last.request.headers["traceparent"] == tp
    c.close()


@respx.mock
def test_trace_3_no_trace_when_no_provider(api_base: str) -> None:
    route = respx.get(_u(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090")
    c._request("GET", PROBE)
    assert "traceparent" not in route.calls.last.request.headers
    c.close()


@respx.mock
def test_trace_4_provider_exception_swallowed(api_base: str) -> None:
    def boom() -> str:
        raise RuntimeError("provider down")

    route = respx.get(_u(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090", trace_id_provider=boom)
    c._request("GET", PROBE)  # must not raise
    assert "traceparent" not in route.calls.last.request.headers
    c.close()


@respx.mock
async def test_trace_async_shares_header_logic(api_base: str) -> None:
    """trace injection also works on the async client (shared _build_headers)."""
    tp = "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01"
    route = respx.get(_u(api_base)).mock(return_value=httpx.Response(200, json={}))
    c = AsyncCortrix(
        base_url="http://testserver:9090", client_id="async-app", trace_id_provider=lambda: tp
    )
    await c._request("GET", PROBE)
    req = route.calls.last.request
    assert req.headers["X-Client-Id"] == "async-app"
    assert req.headers["traceparent"] == tp
    await c.close()
