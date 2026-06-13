"""Memory resource tests (/memory domain, real-arch wire)."""

from __future__ import annotations

import json

import httpx
import respx

from cortrix import Cortrix
from cortrix.types import Memory, MemoryList

_MEM = {"memory_id": "m1", "content": "x", "memory_type": "fact", "status": "valid"}


@respx.mock
def test_search_posts_to_memory_search(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/memory/search").mock(
        return_value=httpx.Response(200, json={"memories": [_MEM]})
    )
    out = client.memory.search("user_memory", "last progress", user_id="u1", top_k=3)
    assert isinstance(out, MemoryList) and out.memories[0].memory_id == "m1"
    body = json.loads(route.calls.last.request.content)
    assert body == {"namespace": "user_memory", "query": "last progress", "user_id": "u1", "top_k": 3}


@respx.mock
def test_log_posts_to_session_interactions(api_base: str, client: Cortrix) -> None:
    # Live wire: interactions are written under /memory/sessions/{id}/interactions
    # (the designed one-shot /memory/log is not mounted yet -> D3.5).
    route = respx.post(api_base + "/memory/sessions/s1/interactions").mock(
        return_value=httpx.Response(200, json={"interaction_id": "i1"})
    )
    client.memory.log("ns", query="q", response="r", user_id="u1", session_id="s1")
    body = json.loads(route.calls.last.request.content)
    assert body["namespace"] == "ns"
    assert body["query_text"] == "q" and body["response_text"] == "r"
    assert body["user_id"] == "u1"


def test_log_requires_session_id(api_base: str, client: Cortrix) -> None:
    try:
        client.memory.log("ns", query="q", response="r", user_id="u1")
        raise AssertionError("expected ValueError")
    except ValueError as err:
        assert "session_id" in str(err)


@respx.mock
def test_extract_posts_to_memory_extract(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/memory/extract").mock(
        return_value=httpx.Response(200, json={"succeeded": [], "failed": []})
    )
    client.memory.extract("ns", query="q", response="r", user_id="u1")
    assert route.calls.last.request.url.path == "/api/v1/memory/extract"


@respx.mock
def test_list_params(api_base: str, client: Cortrix) -> None:
    route = respx.get(api_base + "/memory").mock(
        return_value=httpx.Response(200, json={"memories": [_MEM], "total": 1})
    )
    out = client.memory.list(namespace="ns", include_invalidated=True, memory_type="fact")
    assert len(out) == 1
    p = route.calls.last.request.url.params
    assert p["namespace"] == "ns" and p["include_invalidated"] == "true" and p["memory_type"] == "fact"


@respx.mock
def test_create(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/memory").mock(return_value=httpx.Response(201, json=_MEM))
    m = client.memory.create("ns", "User prefers Markdown", memory_type="preference")
    assert isinstance(m, Memory)
    body = json.loads(route.calls.last.request.content)
    assert body == {"namespace": "ns", "content": "User prefers Markdown", "memory_type": "preference"}


@respx.mock
def test_update_is_patch(api_base: str, client: Cortrix) -> None:
    route = respx.patch(api_base + "/memory/m1").mock(return_value=httpx.Response(200, json=_MEM))
    client.memory.update("m1", content="new")
    assert route.calls.last.request.method == "PATCH"
    assert json.loads(route.calls.last.request.content) == {"content": "new"}


@respx.mock
def test_delete_is_soft_returns_memory(api_base: str, client: Cortrix) -> None:
    inv = {**_MEM, "status": "invalidated"}
    respx.delete(api_base + "/memory/m1").mock(return_value=httpx.Response(200, json=inv))
    m = client.memory.delete("m1")
    assert m.status == "invalidated"


@respx.mock
def test_edit_alias_calls_patch(api_base: str, client: Cortrix) -> None:
    route = respx.patch(api_base + "/memory/m2").mock(return_value=httpx.Response(200, json=_MEM))
    client.memory.edit("m2", content="z")
    assert route.calls.last.request.method == "PATCH"


@respx.mock
def test_invalidate_alias_calls_delete(api_base: str, client: Cortrix) -> None:
    route = respx.delete(api_base + "/memory/m3").mock(
        return_value=httpx.Response(200, json={**_MEM, "status": "invalidated"})
    )
    client.memory.invalidate("m3")
    assert route.calls.last.request.method == "DELETE"
