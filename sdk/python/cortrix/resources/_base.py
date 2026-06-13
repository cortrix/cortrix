"""Resource base classes.

A Resource holds a reference to its client and issues requests through the
client's ``_request`` (sync) / awaitable ``_request`` (async). Keeping the
two split lets ``mypy`` see ``await client._request(...)`` on the async side
without ``# type: ignore``.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .._async_base import AsyncBaseClient
    from .._client import Cortrix


class SyncResource:
    """Base for synchronous resources."""

    def __init__(self, client: "Cortrix") -> None:
        self._client = client


class AsyncResource:
    """Base for asynchronous resources."""

    def __init__(self, client: "AsyncBaseClient") -> None:
        self._client = client
