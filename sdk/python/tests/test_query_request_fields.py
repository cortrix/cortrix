"""The query request fields ``search()`` can send, pinned against the server (#171).

Each documented field is sent unchanged when given and left out when not, from both
clients and from ``Query.run()``. The set the SDK sends is pinned in three directions,
each as an exact difference, so a key added on the server, a keyword the server
ignores, or a property added to the specification cannot go unnoticed again:

* the route reads every key the SDK sends, except the listed ones;
* the SDK sends every key the route reads, except the listed ones;
* the SDK sends every request property the specification declares, except the listed one.
"""

from __future__ import annotations

import json
import pathlib
import re

import httpx
import pytest
import respx

from cortrix import AsyncCortrix, Cortrix
from cortrix.resources.query import _query_body

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
QUERY_WIRING = REPO_ROOT / "src" / "query" / "query_wiring.cpp"
QUERY_HANDLER = REPO_ROOT / "src" / "query" / "cross_ns_query_handler.cpp"
SCHEMAS = REPO_ROOT / "api" / "components" / "schemas.yaml"

FIELDS = {
    "route": "complex",
    "rag_fusion": True,
    "include_parent": True,
    "parent_expand_top_k": 3,
    "parent_max_bytes": 4096,
    "explain": True,
}

# Read by the route, not sent by the SDK.
READ_NOT_SENT = {
    "namespace": "the deprecated singular field, which the route rejects",
    "granularity": "not in the specification yet (#183)",
    "crag": "not in the specification yet (#183)",
    "llm_rerank": "not in the specification yet (#183)",
    "llm_rerank_config": "not in the specification yet (#183)",
    "rag_fusion_config": "not in the specification yet (#183)",
    "locale": "not in the specification yet (#183)",
    "timeout_ms": "the SDK does not send it yet (#205)",
}
# Sent by the SDK, not read by the route.
SENT_NOT_READ = {"include_sources": "the route ignores it (#182)"}
# Declared by the specification, not sent by the SDK.
DECLARED_NOT_SENT = {"timeout_ms": "the SDK does not send it yet (#205)"}

_EMPTY = {
    "results": [],
    "meta": {"namespaces_queried": ["ns"], "namespaces_succeeded": ["ns"],
             "coverage_ratio": 1.0, "latency_ms": 1},
}


def _sent(route: respx.Route) -> dict:
    return json.loads(route.calls.last.request.content)


# --- each field goes out unchanged, and only when given ------------------------------

@respx.mock
@pytest.mark.parametrize("field,value", sorted(FIELDS.items()))
def test_search_sends_each_field_unchanged(
    api_base: str, client: Cortrix, field: str, value: object
) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_EMPTY))
    client.search("ns", "q", **{field: value})
    body = _sent(route)
    assert body[field] == value
    assert {k for k in FIELDS if k in body} == {field}


@respx.mock
@pytest.mark.parametrize("field,value", sorted(FIELDS.items()))
async def test_async_search_sends_each_field_unchanged(
    api_base: str, aclient: AsyncCortrix, field: str, value: object
) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_EMPTY))
    await aclient.search("ns", "q", **{field: value})
    body = _sent(route)
    assert body[field] == value
    assert {k for k in FIELDS if k in body} == {field}


@respx.mock
def test_query_run_sends_every_field(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_EMPTY))
    client.query.run("ns", "q", **FIELDS)
    body = _sent(route)
    assert {k: body[k] for k in FIELDS} == FIELDS


@respx.mock
async def test_async_query_run_sends_every_field(api_base: str, aclient: AsyncCortrix) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_EMPTY))
    await aclient.query.run("ns", "q", **FIELDS)
    body = _sent(route)
    assert {k: body[k] for k in FIELDS} == FIELDS


@respx.mock
async def test_omitted_fields_are_not_sent(
    api_base: str, client: Cortrix, aclient: AsyncCortrix
) -> None:
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_EMPTY))
    client.search("ns", "q")
    assert set(FIELDS) & set(_sent(route)) == set()
    await aclient.search("ns", "q")
    assert set(FIELDS) & set(_sent(route)) == set()


@respx.mock
def test_false_and_zero_are_sent_not_dropped(api_base: str, client: Cortrix) -> None:
    """Only ``None`` means "not given": an explicit false or 0 reaches the server."""
    route = respx.post(api_base + "/query").mock(return_value=httpx.Response(200, json=_EMPTY))
    client.search("ns", "q", rag_fusion=False, include_parent=False,
                  parent_expand_top_k=0, parent_max_bytes=0, explain=False)
    body = _sent(route)
    assert (body["rag_fusion"], body["include_parent"], body["explain"]) == (False, False, False)
    assert (body["parent_expand_top_k"], body["parent_max_bytes"]) == (0, 0)


# --- the sent set, pinned against the server and the specification -------------------

def _route_reads() -> set[str]:
    """Top-level body keys the served route reads: ``body.contains("k")``,
    ``body["k"]`` and ``ReadIntControl(req, body, "k", ...)`` in the route's file and
    in the request parser. Keys of nested objects (``search_config``'s flags, the
    two ``*_config`` objects) are read through other names and are not collected."""
    if not QUERY_WIRING.exists() or not QUERY_HANDLER.exists():
        pytest.skip("server sources not present (installed package, not the repository)")
    text = (QUERY_WIRING.read_text(encoding="utf-8") + "\n"
            + QUERY_HANDLER.read_text(encoding="utf-8"))
    keys = set(re.findall(r'\bbody(?:\.contains\(|\[)"([a-z0-9_]+)"', text))
    keys |= set(re.findall(r'ReadIntControl\(req, body, "([a-z0-9_]+)"', text))
    # Positive controls: keys certainly read, so an empty or wrong scan cannot pass.
    assert {"query", "namespaces", "top_k", "route", "granularity",
            "parent_max_bytes"} <= keys
    return keys


def _sdk_sends() -> set[str]:
    return set(_query_body("ns", "q", 10, True, True, {"k": "v"}, {"enable_bm25": False},
                           **FIELDS))


def test_the_route_reads_every_key_the_sdk_sends() -> None:
    assert _sdk_sends() - _route_reads() == set(SENT_NOT_READ)


def test_the_sdk_sends_every_key_the_route_reads() -> None:
    assert _route_reads() - _sdk_sends() == set(READ_NOT_SENT)


def test_the_sdk_sends_every_declared_request_property() -> None:
    yaml = pytest.importorskip("yaml", reason="PyYAML (sdk/python[dev]) reads the spec")
    if not SCHEMAS.exists():
        pytest.skip("spec not present (installed package, not the repository)")
    schemas = yaml.safe_load(SCHEMAS.read_text(encoding="utf-8"))["components"]["schemas"]
    declared = set(schemas["QueryRequest"]["properties"])
    assert {"query", "namespaces", "route"} <= declared  # positive control
    assert declared - _sdk_sends() == set(DECLARED_NOT_SENT)
