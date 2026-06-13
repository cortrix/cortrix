"""Ops namespace (``client.ops.gc.*``) — design § 2.13 (issue 2 C).

Phase 1 contains ``gc`` (GcOps). Phase 2 evolution hooks: ``admin`` / ``tenant``.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Optional

from .gc import AsyncGcOps, GcOps

if TYPE_CHECKING:
    from ..._async_base import AsyncBaseClient
    from ..._client import Cortrix


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


__all__ = ["OpsNamespace", "AsyncOpsNamespace", "GcOps", "AsyncGcOps"]
