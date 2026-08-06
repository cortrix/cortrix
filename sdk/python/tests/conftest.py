"""Shared pytest fixtures.

Unit tests mock the HTTP layer with ``respx`` (intercepts httpx). No real
Cortrix Server is started — the ~10 real-server integration tests are deferred
to integration (B-R2 standalone rule).
"""

from __future__ import annotations

import pytest

from cortrix import AsyncCortrix, Cortrix

BASE_URL = "http://testserver:9090"
API_PREFIX = "/api/v1"


@pytest.fixture
def base_url() -> str:
    return BASE_URL


@pytest.fixture
def api_base() -> str:
    """base_url + API prefix — the root every request is mounted under."""
    return BASE_URL + API_PREFIX


@pytest.fixture
def client() -> Cortrix:
    """Sync client with a small max_retries so retry tests run fast."""
    c = Cortrix(base_url=BASE_URL, api_key="cx_test_key", max_retries=2)
    yield c
    c.close()


@pytest.fixture
async def aclient() -> AsyncCortrix:
    c = AsyncCortrix(base_url=BASE_URL, api_key="cx_test_key", max_retries=2)
    yield c
    await c.close()
