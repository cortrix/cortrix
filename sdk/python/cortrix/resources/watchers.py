"""Watchers resource. ``/watch`` domain (ARCH § 4.1.4 + F21/F42).

Per Derek's ruling (briefing § 9): SDK shape = P03 § 2.12; wire = real
architecture. ``add()`` follows the real-arch fan-out shape (``path`` +
``target_namespaces`` array + ``recursive``); the design's per-watcher
``patterns`` / ``auto_delete`` are reconciled in D3.5.
"""

from __future__ import annotations

from typing import Any, List

from ..types import Watcher
from ..types.lists import WatcherList
from ._base import AsyncResource, SyncResource

PATH_WATCH = "/watch"
PATH_WATCH_ID = "/watch/{id}"
PATH_WATCH_EVENTS = "/watch/{id}/events"


def _add_body(path: str, namespaces: List[str], recursive: bool) -> dict[str, Any]:
    return {"path": path, "target_namespaces": namespaces, "recursive": recursive}


class Watchers(SyncResource):
    """File-watcher management. ``/watch`` domain."""

    def add(
        self, path: str, namespaces: List[str], *, recursive: bool = True
    ) -> Watcher:
        """Add/append a directory watcher. ``POST /watch`` -> 201 Watcher.

        ``namespaces`` = the fan-out target NS list (spec ``target_namespaces``).
        """
        return self._client._request(
            "POST", PATH_WATCH, json=_add_body(path, namespaces, recursive),
            response_model=Watcher,
        )

    def list(self) -> WatcherList:
        """List watchers. ``GET /watch``."""
        return self._client._request("GET", PATH_WATCH, response_model=WatcherList)

    def remove(self, watcher_id: str) -> None:
        """Remove a watcher. ``DELETE /watch/{id}`` -> 204."""
        self._client._request("DELETE", PATH_WATCH_ID.format(id=watcher_id))

    def events(self, watcher_id: str, *, limit: int = 50) -> Any:
        """Get a watcher's event stream. ``GET /watch/{id}/events`` -> {events}."""
        return self._client._request(
            "GET", PATH_WATCH_EVENTS.format(id=watcher_id), params={"limit": limit}
        )


class AsyncWatchers(AsyncResource):
    """Async Watchers (symmetric to :class:`Watchers`)."""

    async def add(
        self, path: str, namespaces: List[str], *, recursive: bool = True
    ) -> Watcher:
        return await self._client._request(
            "POST", PATH_WATCH, json=_add_body(path, namespaces, recursive),
            response_model=Watcher,
        )

    async def list(self) -> WatcherList:
        return await self._client._request("GET", PATH_WATCH, response_model=WatcherList)

    async def remove(self, watcher_id: str) -> None:
        await self._client._request("DELETE", PATH_WATCH_ID.format(id=watcher_id))

    async def events(self, watcher_id: str, *, limit: int = 50) -> Any:
        return await self._client._request(
            "GET", PATH_WATCH_EVENTS.format(id=watcher_id), params={"limit": limit}
        )
