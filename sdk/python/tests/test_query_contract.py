"""The typed query result must describe what ``POST /query`` actually sends (#146).

Three directions are pinned, so a field the server writes cannot again be dropped
silently by the tolerant parser (``parse_model`` ignores unknown keys by design):

* server source -> model: the keys the server's serializers write are fields of
  the typed result;
* spec -> model: every property the spec declares for the result types is a field;
* wire -> typed: a response carrying those fields parses with each value visible.

The request side is pinned too: ``search_config`` can be sent.
"""

from __future__ import annotations

import dataclasses
import json
import pathlib
import re

import httpx
import pytest
import respx

from cortrix import AsyncCortrix, Cortrix
from cortrix._models import parse_model
from cortrix.types import QueryMeta, QueryResult, QueryResultItem

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
CROSS_NS_RESPONSE = REPO_ROOT / "src" / "query" / "cross_ns_response.cpp"
QUERY_WIRING = REPO_ROOT / "src" / "query" / "query_wiring.cpp"
SCHEMAS = REPO_ROOT / "api" / "components" / "schemas.yaml"


def _fields(model) -> set[str]:
    return {f.name for f in dataclasses.fields(model)}


def _function_body(source: str, definition: str) -> str:
    """The text of one function: from its definition line to the next closing brace
    at column 0. Anchored on the definition (``^`` + return type), not on the bare
    name, which also appears at call sites."""
    m = re.search(r"^" + re.escape(definition), source, re.M)
    assert m, f"definition not found: {definition}"
    end = source.find("\n}\n", m.end())
    assert end != -1, f"no closing brace after {definition}"
    return source[m.start():end]


def _keys(text: str, pattern: str) -> set[str]:
    return set(re.findall(pattern, text))


def _server_sources() -> tuple[str, str]:
    if not CROSS_NS_RESPONSE.exists() or not QUERY_WIRING.exists():
        pytest.skip("server sources not present (installed package, not the repository)")
    return (CROSS_NS_RESPONSE.read_text(encoding="utf-8"),
            QUERY_WIRING.read_text(encoding="utf-8"))


# --- server source -> model -------------------------------------------------------------

def test_every_key_the_server_writes_is_a_typed_field() -> None:
    """Keys written by the cross-namespace serializers (``cross_ns_response.cpp``:
    each result item, the meta object, the top level) and by the route
    (``query_wiring.cpp``: the chat route's meta, the route's additions to meta, its
    top-level additions) must all be fields of the typed result.

    Limits, stated so they are not mistaken for coverage: the chat route's result
    items are built with an initializer list (``query_wiring.cpp``), which these
    patterns do not read, so their shape (``child_id``, ``content``, ``memory_type``,
    ``created_at``, ``confidence``) is not checked here; and the ``meta[...]`` and
    ``out[...]`` scans run over the whole of ``query_wiring.cpp``, where today only
    the chat builder and the route handler write those names."""
    cross_ns, wiring = _server_sources()

    item = _keys(_function_body(cross_ns, "nlohmann::json ResultItemToJson("),
                 r'\bj\["([a-z0-9_]+)"\]\s*=')
    meta = _keys(_function_body(cross_ns, "nlohmann::json CrossNsMeta::ToJson() const"),
                 r'\bj\["([a-z0-9_]+)"\]\s*=')
    top = _keys(_function_body(cross_ns, "nlohmann::json CrossNsResponse::ToJson() const"),
                r'\bj\["([a-z0-9_]+)"\]\s*=')
    chat_meta = _keys(wiring, r'\bmeta\["([a-z0-9_]+)"\]\s*=')
    route_meta = _keys(wiring, r'\bout\["meta"\]\["([a-z0-9_]+)"\]\s*=')
    route_top = _keys(wiring, r'\bout\["([a-z0-9_]+)"\]\s*=')

    # Positive controls: each extraction found what is certainly there, so an empty
    # or wrong slice cannot pass for "no missing keys".
    assert {"child_id", "content", "evidence_kind"} <= item
    assert {"namespaces_queried", "coverage_ratio", "latency_ms"} <= meta
    assert {"results", "meta", "error"} <= top
    assert {"namespaces_queried", "via_path"} <= chat_meta
    assert {"crag_verdict", "crag_action"} <= route_meta
    assert {"results", "meta", "explain"} <= route_top

    assert item - _fields(QueryResultItem) == set(), "result item keys missing from QueryResultItem"
    assert (meta | chat_meta | route_meta) - _fields(QueryMeta) == set(), \
        "meta keys missing from QueryMeta"
    assert (top | route_top) - _fields(QueryResult) == set(), \
        "top-level keys missing from QueryResult"


# --- spec -> model --------------------------------------------------------------------------

def test_every_property_the_spec_declares_is_a_typed_field() -> None:
    yaml = pytest.importorskip("yaml", reason="PyYAML (sdk/python[dev]) reads the spec")
    if not SCHEMAS.exists():
        pytest.skip("spec not present (installed package, not the repository)")
    schemas = yaml.safe_load(SCHEMAS.read_text(encoding="utf-8"))["components"]["schemas"]
    for name, model in (("QueryResultItem", QueryResultItem), ("QueryMeta", QueryMeta),
                        ("QueryResult", QueryResult)):
        declared = set(schemas[name]["properties"])
        assert declared - _fields(model) == set(), f"{name}: declared but not typed"
    # The dedup entries are objects on the wire; the spec once said strings.
    dedup = schemas["QueryMeta"]["properties"]["deduplicated_chunks"]
    assert dedup["items"]["type"] == "object"
    assert {"content_hash", "primary_namespace", "namespaces"} <= set(dedup["items"]["properties"])


# --- wire -> typed --------------------------------------------------------------------------

_RETRIEVAL = {
    "results": [{
        "child_id": "c1", "parent_id": "p1", "content": "a passage",
        "parent_content": "", "score": 0.9, "rerank_score": 0.95, "namespace": "ns1",
        "content_hash": "sha256:00", "metadata": {"doc_id": "d1"},
        "evidence_kind": "chunk",
        "score_signals": {"enriched_score": 0.5, "semantic_score": 0.75},
    }],
    "meta": {
        "namespaces_queried": ["ns1", "ns2"], "namespaces_succeeded": ["ns1"],
        "namespaces_failed": [{
            "namespace": "ns2", "error_code": "CX_ERR_INDEX_CORRUPT", "retryable": False,
            "category": "permanent", "retry_after_ms": None, "message": "x",
            "structured_data": {"namespace": "ns2", "index_state": "corrupt"},
        }],
        "coverage_ratio": 0.5, "latency_ms": 12,
        "deduplicated_chunks": [{
            "content_hash": "sha256:00", "primary_namespace": "ns1",
            "namespaces": [{"namespace": "ns1", "score": 0.9, "rerank_score": 0.95}],
        }],
        "deduplicated_chunks_count": 1, "warnings": [],
        "crag_verdict": "ambiguous", "crag_action": "refine",
    },
    "explain": {"granularity": "chunk"},
    "error": {"code": "CX_ERR_SCATTER_TIMEOUT", "message": "budget", "retryable": True,
              "category": "timeout", "retry_after_ms": 100, "structured_data": None},
}

_CHAT = {
    "results": [{"child_id": "m1", "content": "remembered", "memory_type": "fact",
                 "created_at": "2026-10-16T00:00:00Z", "confidence": 0.8}],
    "meta": {"namespaces_queried": ["ns1"], "namespaces_succeeded": ["ns1"],
             "namespaces_failed": [], "coverage_ratio": 1.0, "latency_ms": 0,
             "deduplicated_chunks": [], "deduplicated_chunks_count": 0, "warnings": [],
             "via_path": "chat_memory_only"},
}


def _check_retrieval(res: QueryResult) -> None:
    item = res.results[0]
    assert item.evidence_kind == "chunk"
    assert item.score_signals == {"enriched_score": 0.5, "semantic_score": 0.75}
    assert res.meta.crag_verdict == "ambiguous" and res.meta.crag_action == "refine"
    assert res.meta.namespaces_failed[0]["structured_data"]["index_state"] == "corrupt"
    assert res.meta.deduplicated_chunks[0]["primary_namespace"] == "ns1"
    assert res.explain == {"granularity": "chunk"}
    assert res.error["code"] == "CX_ERR_SCATTER_TIMEOUT"


def test_retrieval_response_parses_with_every_field_visible() -> None:
    _check_retrieval(parse_model(QueryResult, _RETRIEVAL))


@respx.mock
def test_search_returns_every_field_the_server_sent(api_base: str, client: Cortrix) -> None:
    respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_RETRIEVAL))
    _check_retrieval(client.search("ns1", "q"))


@respx.mock
def test_chat_route_response_parses(api_base: str, client: Cortrix) -> None:
    # The chat route's items have their own shape; the typed item keeps what it
    # shares (child_id, content) and drops the rest without failing.
    respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_CHAT))
    res = client.search("ns1", "q")
    assert res.meta.via_path == "chat_memory_only"
    assert res.results[0].child_id == "m1" and res.results[0].content == "remembered"
    assert res.results[0].evidence_kind is None


# --- request side ---------------------------------------------------------------------------

def test_search_config_declares_exactly_the_flags_the_server_reads() -> None:
    """The served route reads each ``search_config`` flag by name
    (``query_wiring.cpp``, ``sc.contains("...")``); the spec and the typed request
    object must declare exactly those flags. A flag the route never reads is a toggle
    that silently does nothing; a flag it reads but the spec omits cannot be sent."""
    yaml = pytest.importorskip("yaml", reason="PyYAML (sdk/python[dev]) reads the spec")
    _, wiring = _server_sources()
    if not SCHEMAS.exists():
        pytest.skip("spec not present (installed package, not the repository)")
    # Positive control: `sc` is the search_config object, declared once.
    assert wiring.count('const auto& sc = body["search_config"];') == 1
    assert len(re.findall(r"\bconst auto& sc\b", wiring)) == 1
    read = _keys(wiring, r'\bsc\.contains\("([a-z0-9_]+)"\)')
    assert {"enable_vector", "enable_bm25"} <= read

    spec = yaml.safe_load(SCHEMAS.read_text(encoding="utf-8"))["components"]["schemas"]
    assert set(spec["SearchConfig"]["properties"]) == read

    from cortrix.types import SearchConfig  # imported here: absent before #146

    assert _fields(SearchConfig) == read


@respx.mock
def test_search_sends_search_config(api_base: str, client: Cortrix) -> None:
    from cortrix.types import SearchConfig  # imported here: absent before #146

    route = respx.post(api_base + "/query").mock(
        return_value=httpx.Response(200, json=_RETRIEVAL))

    client.search("ns1", "q", search_config=SearchConfig(enable_sparse=False))
    body = json.loads(route.calls.last.request.content)
    assert body["search_config"] == {"enable_sparse": False}  # unset toggles left to the server

    client.search("ns1", "q", search_config={"enable_vector": True, "enable_bm25": False})
    body = json.loads(route.calls.last.request.content)
    assert body["search_config"] == {"enable_vector": True, "enable_bm25": False}

    client.search("ns1", "q")
    assert "search_config" not in json.loads(route.calls.last.request.content)

    with pytest.raises(TypeError):
        client.search("ns1", "q", search_config="enable_bm25=false")  # type: ignore[arg-type]


@respx.mock
async def test_async_search_sends_search_config() -> None:
    from cortrix.types import SearchConfig  # imported here: absent before #146

    base = "http://testserver:9090"
    route = respx.post(base + "/api/v1/query").mock(
        return_value=httpx.Response(200, json=_RETRIEVAL))
    c = AsyncCortrix(base_url=base, api_key="k")
    try:
        res = await c.search("ns1", "q", search_config=SearchConfig(enable_bm25=False))
    finally:
        await c.close()
    assert json.loads(route.calls.last.request.content)["search_config"] == {"enable_bm25": False}
    _check_retrieval(res)
