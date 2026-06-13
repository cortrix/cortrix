"""Client init / config / context-manager tests."""

from __future__ import annotations

import httpx
import respx

from cortrix import AsyncCortrix, Cortrix
from cortrix._constants import API_PREFIX, DEFAULT_BASE_URL, DEFAULT_TIMEOUT, SDK_VERSION


def test_init_default_config() -> None:
    c = Cortrix()
    assert c.base_url == DEFAULT_BASE_URL.rstrip("/")
    assert c.timeout == DEFAULT_TIMEOUT
    assert c.api_key is None
    c.close()


def test_init_custom_config() -> None:
    c = Cortrix(
        base_url="https://api.cortrix.io/",
        api_key="cx_live_x",
        tenant_id="t-1",
        timeout=10.0,
        max_retries=5,
        client_id="app",
    )
    assert c.base_url == "https://api.cortrix.io"  # trailing slash stripped
    assert c.api_key == "cx_live_x" and c.tenant_id == "t-1"
    assert c.timeout == 10.0 and c.max_retries == 5 and c.client_id == "app"
    c.close()


def test_url_join() -> None:
    c = Cortrix(base_url="http://h:9090")
    assert c._url("/query") == f"http://h:9090{API_PREFIX}/query"
    assert c._url("documents") == f"http://h:9090{API_PREFIX}/documents"
    c.close()


@respx.mock
def test_custom_http_client_is_used_and_not_closed(api_base: str) -> None:
    respx.get(api_base + "/_p").mock(return_value=httpx.Response(200, json={}))
    custom = httpx.Client()
    c = Cortrix(base_url="http://testserver:9090", http_client=custom)
    c._request("GET", "/_p")
    c.close()  # must NOT close a caller-provided client
    assert not custom.is_closed
    custom.close()


def test_context_manager_sync() -> None:
    with Cortrix(base_url="http://h:9090") as c:
        assert isinstance(c, Cortrix)
        http = c._http
    assert http.is_closed  # __exit__ closed the owned client


async def test_context_manager_async() -> None:
    async with AsyncCortrix(base_url="http://h:9090") as c:
        assert isinstance(c, AsyncCortrix)
        http = c._http
    assert http.is_closed


@respx.mock
def test_user_agent_header(api_base: str) -> None:
    route = respx.get(api_base + "/_p").mock(return_value=httpx.Response(200, json={}))
    c = Cortrix(base_url="http://testserver:9090")
    c._request("GET", "/_p")
    assert route.calls.last.request.headers["User-Agent"] == f"cortrix-python/{SDK_VERSION}"
    c.close()
