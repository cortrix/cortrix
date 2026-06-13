"""Async base client.

The header / exception / retry-decision helpers are pure (I/O-free) and live on
:class:`cortrix._base_client.BaseClient`; the async client reuses them verbatim
(design § 2.13.1 architecture note: no separate async ``_build_headers``).
``AsyncBaseClient`` exists so async Resources can type their ``_client`` against
an async-specific base whose ``_request`` is a coroutine. The overloads mirror
:class:`cortrix._async_client.AsyncCortrix` so ``await client._request(...,
response_model=X)`` is seen as returning ``X``.
"""

from __future__ import annotations

from typing import Any, Optional, Type, TypeVar, overload

from ._base_client import BaseClient

T = TypeVar("T")


class AsyncBaseClient(BaseClient):
    """Marker base for the async client (shares all pure helpers from BaseClient)."""

    @overload
    async def _request(
        self,
        method: str,
        path: str,
        *,
        json: Optional[dict[str, Any]] = ...,
        params: Optional[dict[str, Any]] = ...,
        files: Optional[dict[str, Any]] = ...,
        data: Optional[dict[str, Any]] = ...,
        headers: Optional[dict[str, str]] = ...,
        timeout: Optional[float] = ...,
        response_model: Type[T],
    ) -> T: ...

    @overload
    async def _request(
        self,
        method: str,
        path: str,
        *,
        json: Optional[dict[str, Any]] = ...,
        params: Optional[dict[str, Any]] = ...,
        files: Optional[dict[str, Any]] = ...,
        data: Optional[dict[str, Any]] = ...,
        headers: Optional[dict[str, str]] = ...,
        timeout: Optional[float] = ...,
        response_model: None = ...,
    ) -> Any: ...

    async def _request(  # pragma: no cover - overridden by AsyncCortrix
        self,
        method: str,
        path: str,
        *,
        json: Optional[dict[str, Any]] = None,
        params: Optional[dict[str, Any]] = None,
        files: Optional[dict[str, Any]] = None,
        data: Optional[dict[str, Any]] = None,
        headers: Optional[dict[str, str]] = None,
        timeout: Optional[float] = None,
        response_model: Optional[Type[T]] = None,
    ) -> Any:
        raise NotImplementedError
