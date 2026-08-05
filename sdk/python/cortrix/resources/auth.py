"""Auth resource. ``/auth/*`` (ARCH § 4.1.9 + auth).

This resource follows the implemented HTTP architecture. It covers
register/login/password_reset plus
logout/refresh/me (commonly needed). Email-verification / api-key CRUD endpoints
exist in the real-arch surface and can be added if § 2.12 scope expands (D3.5).
"""

from __future__ import annotations

from typing import Any, Optional

from ..types import LoginResponse, User
from ._base import AsyncResource, SyncResource

PATH_REGISTER = "/auth/register"
PATH_LOGIN = "/auth/login"
PATH_LOGOUT = "/auth/logout"
PATH_REFRESH = "/auth/refresh"
PATH_PASSWORD_RESET = "/auth/password-reset"
PATH_ME = "/auth/me"


def _register_body(email: str, password: str, display_name: Optional[str]) -> dict[str, Any]:
    body: dict[str, Any] = {"email": email, "password": password}
    if display_name is not None:
        body["display_name"] = display_name
    return body


class Auth(SyncResource):
    """Authentication. ``/auth/*``."""

    def register(
        self, email: str, password: str, *, display_name: Optional[str] = None
    ) -> User:
        """Register an account. ``POST /auth/register`` -> User."""
        return self._client._request(
            "POST", PATH_REGISTER, json=_register_body(email, password, display_name),
            response_model=User,
        )

    def login(self, email: str, password: str) -> LoginResponse:
        """Login -> JWT. ``POST /auth/login`` -> LoginResponse."""
        return self._client._request(
            "POST", PATH_LOGIN, json={"email": email, "password": password},
            response_model=LoginResponse,
        )

    def logout(self) -> None:
        """Logout (revoke refresh token). ``POST /auth/logout`` -> 204."""
        self._client._request("POST", PATH_LOGOUT)

    def refresh(self, refresh_token: str) -> LoginResponse:
        """Exchange a refresh token. ``POST /auth/refresh`` -> LoginResponse."""
        return self._client._request(
            "POST", PATH_REFRESH, json={"refresh_token": refresh_token},
            response_model=LoginResponse,
        )

    def password_reset(self, email: str) -> Any:
        """Request a password-reset email. ``POST /auth/password-reset``."""
        return self._client._request("POST", PATH_PASSWORD_RESET, json={"email": email})

    def me(self) -> User:
        """Current user. ``GET /auth/me`` -> User."""
        return self._client._request("GET", PATH_ME, response_model=User)


class AsyncAuth(AsyncResource):
    """Async Auth (symmetric to :class:`Auth`)."""

    async def register(
        self, email: str, password: str, *, display_name: Optional[str] = None
    ) -> User:
        return await self._client._request(
            "POST", PATH_REGISTER, json=_register_body(email, password, display_name),
            response_model=User,
        )

    async def login(self, email: str, password: str) -> LoginResponse:
        return await self._client._request(
            "POST", PATH_LOGIN, json={"email": email, "password": password},
            response_model=LoginResponse,
        )

    async def logout(self) -> None:
        await self._client._request("POST", PATH_LOGOUT)

    async def refresh(self, refresh_token: str) -> LoginResponse:
        return await self._client._request(
            "POST", PATH_REFRESH, json={"refresh_token": refresh_token},
            response_model=LoginResponse,
        )

    async def password_reset(self, email: str) -> Any:
        return await self._client._request("POST", PATH_PASSWORD_RESET, json={"email": email})

    async def me(self) -> User:
        return await self._client._request("GET", PATH_ME, response_model=User)
