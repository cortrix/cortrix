"""Ops namespace (``client.ops.gc.*``) — design (issue 2 C).

Phase 1 contains ``gc`` (GcOps). Phase 2 evolution hooks: ``admin`` / ``tenant``.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Optional

from .gc import AsyncGcOps, GcOps

if TYPE_CHECKING:
    from ..._async_base import AsyncBaseClient
    from ..._client import Cortrix

PATH_OPERATIONS = "/operations"


def _clean_params(params: dict[str, Any]) -> dict[str, Any]:
    return {k: v for k, v in params.items() if v is not None}


class OpsNamespace:
    """Sync ops sub-namespace container. Phase 1: ``gc``."""

    def __init__(self, client: "Cortrix") -> None:
        self._client = client
        self._gc: Optional[GcOps] = None

    @property
    def gc(self) -> GcOps:
        """GC (Garbage Collection) ops endpoints — OPEN-2 (2026-05-01)."""
        if self._gc is None:
            self._gc = GcOps(self._client)
        return self._gc

    def list_operations(
        self,
        *,
        action: Optional[str] = None,
        action_in: Optional[str] = None,
        namespace: Optional[str] = None,
        namespace_id: Optional[str] = None,
        resource_type: Optional[str] = None,
        trace_id: Optional[str] = None,
        user_id: Optional[str] = None,
        from_timestamp: Optional[int] = None,
        to_timestamp: Optional[int] = None,
        limit: Optional[int] = None,
        offset: Optional[int] = None,
        sort_order: Optional[str] = None,
    ) -> Any:
        """List operation-log entries. ``GET /operations``."""
        params = _clean_params(
            {
                "action": action,
                "action_in": action_in,
                "namespace_id": namespace_id or namespace,
                "resource_type": resource_type,
                "trace_id": trace_id,
                "user_id": user_id,
                "from_timestamp": from_timestamp,
                "to_timestamp": to_timestamp,
                "limit": limit,
                "offset": offset,
                "sort_order": sort_order,
            }
        )
        return self._client._request("GET", PATH_OPERATIONS, params=params)


class AsyncOpsNamespace:
    """Async ops sub-namespace container (symmetric to :class:`OpsNamespace`)."""

    def __init__(self, client: "AsyncBaseClient") -> None:
        self._client = client
        self._gc: Optional[AsyncGcOps] = None

    @property
    def gc(self) -> AsyncGcOps:
        if self._gc is None:
            self._gc = AsyncGcOps(self._client)
        return self._gc

    async def list_operations(
        self,
        *,
        action: Optional[str] = None,
        action_in: Optional[str] = None,
        namespace: Optional[str] = None,
        namespace_id: Optional[str] = None,
        resource_type: Optional[str] = None,
        trace_id: Optional[str] = None,
        user_id: Optional[str] = None,
        from_timestamp: Optional[int] = None,
        to_timestamp: Optional[int] = None,
        limit: Optional[int] = None,
        offset: Optional[int] = None,
        sort_order: Optional[str] = None,
    ) -> Any:
        params = _clean_params(
            {
                "action": action,
                "action_in": action_in,
                "namespace_id": namespace_id or namespace,
                "resource_type": resource_type,
                "trace_id": trace_id,
                "user_id": user_id,
                "from_timestamp": from_timestamp,
                "to_timestamp": to_timestamp,
                "limit": limit,
                "offset": offset,
                "sort_order": sort_order,
            }
        )
        return await self._client._request("GET", PATH_OPERATIONS, params=params)


__all__ = ["OpsNamespace", "AsyncOpsNamespace", "GcOps", "AsyncGcOps"]
