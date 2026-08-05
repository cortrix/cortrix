"""Query / search tests (POST /query, cross-NS query wire: ``namespaces`` array)."""

from __future__ import annotations

import json

import httpx
import respx

from cortrix import Cortrix
from cortrix.types import QueryResult

_LIVE = {
    "results": [
        {
            "block_id": 123,
            "block_type": "FILE",
            "chunk_text": "live foo",
            "doc_id": "01DOC",
            "hit_routes": ["vector"],
            "metadata": {},
            "related_blocks_count": 1,
            "score": 0.5,
            "source_path": "a.md",
            "vector_score": 0.9,
        }
    ],
    "meta": {
        "degraded": False,
        "degradation_level": 0,
        "degraded_routes": [],
        "latency_ms": 7,
        "routes_used": ["vector", "bm25"],
        "total_results": 1,
    },
    "sql_result": None,
}

_OK = {
    "results": [
        {"child_id": "c1", "parent_id": "p1", "content": "foo", "score": 0.9, "namespace": "ns1"}
    ],
    "meta": {
        "namespaces_queried": ["ns1"],
        "namespaces_succeeded": ["ns1"],
        "coverage_ratio": 1.0,
        "latency_ms": 12,
    },
}


@respx.mock
def test_search_single_ns_wraps_in_array(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_OK))
    res = client.search("ns1", "Party A breach clause", top_k=5)
    assert isinstance(res, QueryResult)
    assert res.results[0].child_id == "c1"
    assert res.meta.coverage_ratio == 1.0
    body = json.loads(route.calls.last.request.content)
    assert body["namespaces"] == ["ns1"]  # cross-NS query wire: single str -> 1-element array
    assert body["query"] == "Party A breach clause"
    assert body["top_k"] == 5
    assert body["rerank"] is True


@respx.mock
def test_search_cross_ns(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_OK))
    client.search(["ns1", "ns2"], "q")
    assert json.loads(route.calls.last.request.content)["namespaces"] == ["ns1", "ns2"]


@respx.mock
def test_search_all_ns(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_OK))
    client.search(["*"], "q")
    assert json.loads(route.calls.last.request.content)["namespaces"] == ["*"]


@respx.mock
def test_search_adapts_live_wire_fields(api_base: str, client: Cortrix) -> None:
    respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_LIVE))
    res = client.search("ns1", "q")
    item = res.results[0]
    assert item.content == "live foo"          # chunk_text -> content
    assert item.child_id == "123"              # block_id -> child_id (str)
    assert item.parent_id == "01DOC"           # doc_id -> parent_id
    assert item.metadata["source_path"] == "a.md"
    assert res.meta.namespaces_queried == ["ns1"]
    assert res.meta.latency_ms == 7
    assert res.meta.warnings is None


@respx.mock
def test_search_with_filters_maps_to_singular_filter(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_OK))
    client.search("ns1", "q", filters={"tags": ["x"]})
    body = json.loads(route.calls.last.request.content)
    assert body["filter"] == {"tags": ["x"]}  # spec field is singular ``filter``
    assert "filters" not in body


@respx.mock
def test_search_iterable_and_len(api_base: str, client: Cortrix) -> None:
    respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_OK))
    res = client.search("ns1", "q")
    assert len(res.results) == 1
    assert [r.content for r in res.results] == ["foo"]


@respx.mock
def test_search_include_sources_flag(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_OK))
    client.search("ns1", "q", include_sources=True)
    assert json.loads(route.calls.last.request.content)["include_sources"] is True
