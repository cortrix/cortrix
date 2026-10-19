"""client.auth's account-login calls against a server that authenticates with API keys (#172).

The server in this repository provides ``/auth/me`` and the API-key routes, not account
login. It answers the other five calls with a 404, which the SDK reports as
``FeatureNotAvailableError`` naming the call, instead of a bare not-found error.
"""

from __future__ import annotations

import httpx
import pytest
import respx

from cortrix import (
    AsyncCortrix,
    AuthenticationError,
    Cortrix,
    FeatureNotAvailableError,
    NotFoundError,
)

# What the server writes for a route it does not have.
_ROUTE_MISSING = {"error": {"code": "NOT_FOUND", "message": "The requested resource was not found"}}

ACCOUNT_CALLS = [
    ("register", ("u@e.com", "pw"), "/auth/register"),
    ("login", ("u@e.com", "pw"), "/auth/login"),
    ("logout", (), "/auth/logout"),
    ("refresh", ("r",), "/auth/refresh"),
    ("password_reset", ("u@e.com",), "/auth/password-reset"),
]


def _check(exc: FeatureNotAvailableError, path: str) -> None:
    assert f"POST {path}" in str(exc) and "API keys" in str(exc)
    assert exc.status_code == 404 and exc.retryable is False
    assert isinstance(exc.__cause__, NotFoundError)


@respx.mock
@pytest.mark.parametrize(("name", "args", "path"), ACCOUNT_CALLS)
def test_account_call_says_it_is_unavailable(
    api_base: str, client: Cortrix, name: str, args: tuple, path: str
) -> None:
    respx.post(api_base + path).mock(return_value=httpx.Response(404, json=_ROUTE_MISSING))
    with pytest.raises(FeatureNotAvailableError) as info:
        getattr(client.auth, name)(*args)
    _check(info.value, path)


@respx.mock
@pytest.mark.parametrize(("name", "args", "path"), ACCOUNT_CALLS)
async def test_async_account_call_says_it_is_unavailable(
    api_base: str, aclient: AsyncCortrix, name: str, args: tuple, path: str
) -> None:
    respx.post(api_base + path).mock(return_value=httpx.Response(404, json=_ROUTE_MISSING))
    with pytest.raises(FeatureNotAvailableError) as info:
        await getattr(aclient.auth, name)(*args)
    _check(info.value, path)


@respx.mock
def test_me_keeps_its_not_found(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/auth/me").mock(return_value=httpx.Response(404, json=_ROUTE_MISSING))
    with pytest.raises(NotFoundError):
        client.auth.me()


@respx.mock
def test_other_errors_pass_through(api_base: str, client: Cortrix) -> None:
    body = {"error": {"code": "CX_ERR_AUTH_INVALID_CREDENTIALS", "message": "bad credentials"}}
    respx.post(api_base + "/auth/login").mock(return_value=httpx.Response(401, json=body))
    with pytest.raises(AuthenticationError):
        client.auth.login("u@e.com", "pw")
