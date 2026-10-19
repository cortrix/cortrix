"""Auth resource. ``/auth/*``.

``me`` works on every server. ``register``, ``login``, ``logout``, ``refresh`` and
``password_reset`` need a server that provides account login. A server that
authenticates with API keys only, like the server in this repository, answers
them with 404; they then raise :class:`~cortrix.FeatureNotAvailableError`, which
says so, instead of a bare not-found error.
"""

from __future__ import annotations

from typing import Any, Optional

from .._exceptions import FeatureNotAvailableError, NotFoundError
from ..types import LoginResponse, User
from ._base import AsyncResource, SyncResource

PATH_REGISTER = "/auth/register"
PATH_LOGIN = "/auth/login"
PATH_LOGOUT = "/auth/logout"
PATH_REFRESH = "/auth/refresh"
PATH_PASSWORD_RESET = "/auth/password-reset"
PATH_ME = "/auth/me"


def _not_provided(method: str, path: str, exc: NotFoundError) -> FeatureNotAvailableError:
    """The 404 a server without account login gives these calls, said plainly."""
    return FeatureNotAvailableError(
        f"{method} {path} is not available on this server: it authenticates with API "
        "keys (create one with POST /auth/api-keys)",
        status_code=exc.status_code,
        error_code=exc.error_code,
        request_id=exc.request_id,
        body=exc.body,
        retryable=False,
        category="permanent",
    )


def _register_body(email: str, password: str, display_name: Optional[str]) -> dict[str, Any]:
    body: dict[str, Any] = {"email": email, "password": password}
    if display_name is not None:
        body["display_name"] = display_name
    return body


class Auth(SyncResource):
    """Authentication. ``/auth/*``."""

    def _account_call(self, method: str, path: str, **kwargs: Any) -> Any:
        try:
            return self._client._request(method, path, **kwargs)
        except NotFoundError as exc:
            raise _not_provided(method, path, exc) from exc

    def register(
        self, email: str, password: str, *, display_name: Optional[str] = None
    ) -> User:
        """Register an account. ``POST /auth/register`` -> User."""
        return self._account_call(
            "POST", PATH_REGISTER, json=_register_body(email, password, display_name),
            response_model=User,
        )

    def login(self, email: str, password: str) -> LoginResponse:
        """Login -> JWT. ``POST /auth/login`` -> LoginResponse."""
        return self._account_call(
            "POST", PATH_LOGIN, json={"email": email, "password": password},
            response_model=LoginResponse,
        )

    def logout(self) -> None:
        """Logout (revoke refresh token). ``POST /auth/logout`` -> 204."""
        self._account_call("POST", PATH_LOGOUT)

    def refresh(self, refresh_token: str) -> LoginResponse:
        """Exchange a refresh token. ``POST /auth/refresh`` -> LoginResponse."""
        return self._account_call(
            "POST", PATH_REFRESH, json={"refresh_token": refresh_token},
            response_model=LoginResponse,
        )

    def password_reset(self, email: str) -> Any:
        """Request a password-reset email. ``POST /auth/password-reset``."""
        return self._account_call("POST", PATH_PASSWORD_RESET, json={"email": email})

    def me(self) -> User:
        """Current user. ``GET /auth/me`` -> User."""
        return self._client._request("GET", PATH_ME, response_model=User)


class AsyncAuth(AsyncResource):
    """Async Auth (symmetric to :class:`Auth`)."""

    async def _account_call(self, method: str, path: str, **kwargs: Any) -> Any:
        try:
            return await self._client._request(method, path, **kwargs)
        except NotFoundError as exc:
            raise _not_provided(method, path, exc) from exc

    async def register(
        self, email: str, password: str, *, display_name: Optional[str] = None
    ) -> User:
        return await self._account_call(
            "POST", PATH_REGISTER, json=_register_body(email, password, display_name),
            response_model=User,
        )

    async def login(self, email: str, password: str) -> LoginResponse:
        return await self._account_call(
            "POST", PATH_LOGIN, json={"email": email, "password": password},
            response_model=LoginResponse,
        )

    async def logout(self) -> None:
        await self._account_call("POST", PATH_LOGOUT)

    async def refresh(self, refresh_token: str) -> LoginResponse:
        return await self._account_call(
            "POST", PATH_REFRESH, json={"refresh_token": refresh_token},
            response_model=LoginResponse,
        )

    async def password_reset(self, email: str) -> Any:
        return await self._account_call("POST", PATH_PASSWORD_RESET, json={"email": email})

    async def me(self) -> User:
        return await self._client._request("GET", PATH_ME, response_model=User)
