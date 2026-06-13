"""GC ops tests (/gc/*, real-arch wire). T-P03-OPS-1~5."""

from __future__ import annotations

import json

import httpx
import pytest
import respx

from cortrix import Cortrix, ForbiddenError
from cortrix.types import GcStatus

_STATUS = {"status": "idle", "soft_deleted_count": 12, "reclaimable_bytes": 2048}


@respx.mock
def test_gc_status(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/gc/status").mock(return_value=httpx.Response(200, json=_STATUS))
    s = client.ops.gc.status()
    assert isinstance(s, GcStatus) and s.soft_deleted_count == 12


@respx.mock
def test_gc_run_sends_ops_confirm_header(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/gc/run").mock(
        return_value=httpx.Response(202, json={**_STATUS, "status": "running"})
    )
    s = client.ops.gc.run()
    assert s.status == "running"
    assert route.calls.last.request.headers["X-Ops-Confirm"] == "true"


@respx.mock
def test_gc_restore_document_ids(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/gc/restore").mock(
        return_value=httpx.Response(200, json={"succeeded": ["d1"], "failed": []})
    )
    client.ops.gc.restore(["d1", "d2"])
    assert json.loads(route.calls.last.request.content) == {"document_ids": ["d1", "d2"]}


@respx.mock
def test_gc_purge_sends_ops_confirm_header(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/gc/purge").mock(
        return_value=httpx.Response(202, json=_STATUS)
    )
    client.ops.gc.purge()
    assert route.calls.last.request.headers["X-Ops-Confirm"] == "true"


@respx.mock
def test_gc_forbidden_no_admin_carries_category(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/gc/status").mock(
        return_value=httpx.Response(
            403,
            json={"error": {"code": "CX_ERR_AUTH_ADMIN_REQUIRED", "message": "admin only",
                            "retryable": False, "category": "auth"}},
        )
    )
    with pytest.raises(ForbiddenError) as ei:
        client.ops.gc.status()
    assert ei.value.category == "auth"
