"""GcOps — GC ops endpoints. ``/gc/*`` (ops:admin scope).

Per Derek's ruling (briefing § 9): SDK shape = P03 § 2.12; wire = real
architecture (= OPEN-2 GC, captured by the spec). So the GC wire follows the
real-arch shapes (§ 2.13's ``dry_run``/``scope``/``Gc*Result``/``blob_id`` are
the obsoleted D1 draft):
  - ``run()``     -> POST /gc/run  (parameterless + ``X-Ops-Confirm: true``) -> GcStatus
  - ``restore()`` -> POST /gc/restore  (body ``{document_ids: [...]}``) -> PartialSuccessById
  - ``purge()``   -> POST /gc/purge (parameterless + ``X-Ops-Confirm: true``) -> GcStatus
Confirm header = ``X-Ops-Confirm: true`` (real-arch), required by run + purge.
Real-arch defines only ``GcStatus``; blob-level restore / richer result shapes
reconciled in D3.5 (no double ``/api/v1`` prefix — paths are ``/gc/*``).
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, List

from ...types import GcStatus

if TYPE_CHECKING:
    from ..._async_base import AsyncBaseClient
    from ..._client import Cortrix

PATH_GC_STATUS = "/gc/status"
PATH_GC_RUN = "/gc/run"
PATH_GC_RESTORE = "/gc/restore"
PATH_GC_PURGE = "/gc/purge"

# ops destructive confirm header (spec: enum ["true"]).
_OPS_CONFIRM = {"X-Ops-Confirm": "true"}


class GcOps:
    """GC ops endpoints (ops:admin). ``/gc/*``."""

    def __init__(self, client: "Cortrix") -> None:
        self._client = client

    def status(self) -> GcStatus:
        """GC status. ``GET /gc/status`` -> GcStatus."""
        return self._client._request("GET", PATH_GC_STATUS, response_model=GcStatus)

    def run(self) -> GcStatus:
        """Trigger GC. ``POST /gc/run`` (X-Ops-Confirm: true) -> 202 GcStatus."""
        return self._client._request(
            "POST", PATH_GC_RUN, headers=_OPS_CONFIRM, response_model=GcStatus
        )

    def restore(self, document_ids: List[str]) -> Any:
        """Restore soft-deleted documents. ``POST /gc/restore`` -> PartialSuccessById."""
        return self._client._request(
            "POST", PATH_GC_RESTORE, json={"document_ids": document_ids}
        )

    def purge(self) -> GcStatus:
        """Permanently purge soft-deleted data (irreversible).
        ``POST /gc/purge`` (X-Ops-Confirm: true) -> 202 GcStatus."""
        return self._client._request(
            "POST", PATH_GC_PURGE, headers=_OPS_CONFIRM, response_model=GcStatus
        )


class AsyncGcOps:
    """Async GcOps (symmetric to :class:`GcOps`)."""

    def __init__(self, client: "AsyncBaseClient") -> None:
        self._client = client

    async def status(self) -> GcStatus:
        return await self._client._request("GET", PATH_GC_STATUS, response_model=GcStatus)

    async def run(self) -> GcStatus:
        return await self._client._request(
            "POST", PATH_GC_RUN, headers=_OPS_CONFIRM, response_model=GcStatus
        )

    async def restore(self, document_ids: List[str]) -> Any:
        return await self._client._request(
            "POST", PATH_GC_RESTORE, json={"document_ids": document_ids}
        )

    async def purge(self) -> GcStatus:
        return await self._client._request(
            "POST", PATH_GC_PURGE, headers=_OPS_CONFIRM, response_model=GcStatus
        )
