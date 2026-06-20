"""Async client core-path tests (symmetric to the sync resources)."""

from __future__ import annotations

import io

import httpx
import pytest
import respx

from cortrix import AsyncCortrix, AuthenticationError
from cortrix.types import DocumentTask, MemorySearchResponse, QueryResult

_QUERY_OK = {
    "results": [{"child_id": "c1", "content": "x", "score": 0.5, "namespace": "ns"}],
    "meta": {"namespaces_queried": ["ns"], "namespaces_succeeded": ["ns"],
             "coverage_ratio": 1.0, "latency_ms": 5},
}


@respx.mock
async def test_async_search(api_base: str, aclient: AsyncCortrix) -> None:
    respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_QUERY_OK))
    res = await aclient.search("ns", "q")
    assert isinstance(res, QueryResult) and res.results[0].child_id == "c1"


@respx.mock
async def test_async_upload(api_base: str, aclient: AsyncCortrix, tmp_path) -> None:
    p = tmp_path / "a.txt"
    p.write_text("hi", encoding="utf-8")
    respx.post(api_base + "/documents").mock(
        return_value=httpx.Response(202, json={"task_id": "t1", "status": "queued"})
    )
    task = await aclient.documents.upload("ns", str(p))
    assert isinstance(task, DocumentTask) and task.task_id == "t1"


@respx.mock
async def test_async_batch_submit(api_base: str, aclient: AsyncCortrix) -> None:
    import json

    route = respx.post(api_base + "/documents/batch").mock(
        return_value=httpx.Response(
            200,
            json={
                "results": [{"doc_id": "d1", "task_id": "t1", "status": "submitted"}],
                "meta": {"succeeded": ["d1"], "failed": [], "coverage_ratio": 1.0,
                         "total_submitted": 1},
            },
        )
    )
    out = await aclient.documents.batch_submit("ns", [{"doc_id": "d1", "content": "x"}])
    assert out["meta"]["coverage_ratio"] == 1.0
    body = json.loads(route.calls.last.request.content)
    assert body["options"] == {"async": True, "on_duplicate": "skip"}


@respx.mock
async def test_async_memory_search(api_base: str, aclient: AsyncCortrix) -> None:
    respx.post(api_base + "/memory/search").mock(
        return_value=httpx.Response(
            200,
            json={
                "results": [{"block_id": "m1", "content": "x", "score": 0.9}],
                "total_results": 1,
                "latency_ms": 1,
                "degraded": False,
            },
        )
    )
    out = await aclient.memory.search("ns", "q", user_id="u1")
    assert isinstance(out, MemorySearchResponse) and len(out) == 1


@respx.mock
async def test_async_error_handling(api_base: str, aclient: AsyncCortrix) -> None:
    respx.post(api_base + "/query").mock(
        return_value=httpx.Response(401, json={"error": {"code": "CX_ERR_UNAUTH",
                                                         "message": "no", "retryable": False}})
    )
    with pytest.raises(AuthenticationError):
        await aclient.search("ns", "q")


@respx.mock
async def test_async_retry(api_base: str) -> None:
    route = respx.post(api_base + "/query")
    route.side_effect = [
        httpx.Response(503, json={"error": {"code": "CX_ERR_X", "message": "x",
                                            "retryable": True, "retry_after_ms": 5}}),
        httpx.Response(200, json=_QUERY_OK),
    ]
    c = AsyncCortrix(base_url="http://testserver:9090", max_retries=2)
    res = await c.search("ns", "q")
    assert res.results[0].child_id == "c1"
    assert route.call_count == 2
    await c.close()


@respx.mock
async def test_async_ops_gc_run(api_base: str, aclient: AsyncCortrix) -> None:
    route = respx.post(api_base + "/gc/run").mock(
        return_value=httpx.Response(202, json={"status": "running", "soft_deleted_count": 0})
    )
    s = await aclient.ops.gc.run()
    assert s.status == "running"
    assert route.calls.last.request.headers["X-Ops-Confirm"] == "true"


@respx.mock
async def test_async_upload_and_wait(api_base: str, aclient: AsyncCortrix) -> None:
    respx.post(api_base + "/documents").mock(
        return_value=httpx.Response(202, json={"task_id": "t1", "status": "queued"})
    )
    prog = respx.get(api_base + "/documents/tasks/t1/progress")
    prog.side_effect = [
        httpx.Response(200, json={"task_id": "t1", "status": "processing"}),
        httpx.Response(200, json={"task_id": "t1", "status": "ready"}),
    ]
    task = await aclient.documents.upload_and_wait(
        "ns", io.BytesIO(b"hi"), filename="f.txt", poll_interval=0.0
    )
    assert task.status == "ready" and prog.call_count == 2


@respx.mock
async def test_async_memory_log_and_extract(api_base: str, aclient: AsyncCortrix) -> None:
    lg = respx.post(api_base + "/memory/sessions/s1/interactions").mock(
        return_value=httpx.Response(200, json={"interaction_id": "i"})
    )
    ex = respx.post(api_base + "/memory/extract").mock(
        return_value=httpx.Response(200, json={"succeeded": [], "failed": []})
    )
    await aclient.memory.log("ns", query="q", response="r", user_id="u", session_id="s1")
    await aclient.memory.extract("ns", query="q", response="r", user_id="u")
    assert lg.called and ex.called
