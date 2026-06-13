"""Sync resource. ``/sync/*`` (ARCH § 4.1.8 Batch Sync).

Per Derek's ruling (briefing § 9): SDK shape = P03 § 2.12; wire = real
architecture (``start`` / ``status`` / ``stop``):
  - ``configure`` -> POST /sync/start  ({namespace, interval_seconds})
  - ``status``    -> GET  /sync/status (?namespace=)
  - ``stop``      -> POST /sync/stop   ({namespace})
  - ``trigger``   -> POST /sync/trigger (§2.12-only -> D3.5 spec)
The real-arch ``/sync/start`` body is ``{namespace, interval_seconds}`` (the
design's richer ``connection`` / ``sync_config`` dicts reconciled in D3.5).
"""

from __future__ import annotations

from typing import Any, Optional

from ..types import SyncStatus
from ._base import AsyncResource, SyncResource

PATH_SYNC_START = "/sync/start"
PATH_SYNC_STATUS = "/sync/status"
PATH_SYNC_STOP = "/sync/stop"
PATH_SYNC_TRIGGER = "/sync/trigger"  # §2.12-only -> D3.5 spec


class Sync(SyncResource):
    """Batch DB sync (extended deployments). ``/sync/*``."""

    def configure(self, namespace: str, *, interval_seconds: int = 3600) -> SyncStatus:
        """Start scheduled incremental sync. ``POST /sync/start`` -> SyncStatus."""
        return self._client._request(
            "POST",
            PATH_SYNC_START,
            json={"namespace": namespace, "interval_seconds": interval_seconds},
            response_model=SyncStatus,
        )

    def status(self, namespace: Optional[str] = None) -> SyncStatus:
        """Query sync status. ``GET /sync/status`` (omit ns for aggregate)."""
        params = {"namespace": namespace} if namespace is not None else None
        return self._client._request(
            "GET", PATH_SYNC_STATUS, params=params, response_model=SyncStatus
        )

    def stop(self, namespace: str) -> SyncStatus:
        """Stop sync. ``POST /sync/stop`` -> SyncStatus."""
        return self._client._request(
            "POST", PATH_SYNC_STOP, json={"namespace": namespace}, response_model=SyncStatus
        )

    def trigger(self, namespace: str) -> Any:
        """Manually trigger an incremental sync. ``POST /sync/trigger``
        (§2.12-only -> D3.5 spec)."""
        return self._client._request(
            "POST", PATH_SYNC_TRIGGER, json={"namespace": namespace}
        )


class AsyncSync(AsyncResource):
    """Async Sync (symmetric to :class:`Sync`)."""

    async def configure(self, namespace: str, *, interval_seconds: int = 3600) -> SyncStatus:
        return await self._client._request(
            "POST",
            PATH_SYNC_START,
            json={"namespace": namespace, "interval_seconds": interval_seconds},
            response_model=SyncStatus,
        )

    async def status(self, namespace: Optional[str] = None) -> SyncStatus:
        params = {"namespace": namespace} if namespace is not None else None
        return await self._client._request(
            "GET", PATH_SYNC_STATUS, params=params, response_model=SyncStatus
        )

    async def stop(self, namespace: str) -> SyncStatus:
        return await self._client._request(
            "POST", PATH_SYNC_STOP, json={"namespace": namespace}, response_model=SyncStatus
        )

    async def trigger(self, namespace: str) -> Any:
        return await self._client._request(
            "POST", PATH_SYNC_TRIGGER, json={"namespace": namespace}
        )
