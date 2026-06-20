"""Breadth coverage for resource methods not individually exercised elsewhere.

Each case asserts the SDK issues the expected HTTP method + path (body shape is
covered in the per-domain test files). Runs both sync and async so the symmetric
async methods are covered too.
"""

from __future__ import annotations

import io

import httpx
import pytest
import respx

from cortrix import AsyncCortrix, Cortrix

BASE = "http://testserver:9090"
API = BASE + "/api/v1"

# A JSON blob carrying the union of common required fields across response models,
# so a generic mock parses into any of them (breadth coverage, not shape checks).
_SWEEP_JSON = {
    "status": "active", "namespace": "n", "task_id": "t", "id": "w1",
    "tenant_id": "t1", "name": "x", "memory_id": "m", "content": "c",
    "memory_type": "fact", "document_id": "d", "watchers": [], "memories": [],
    "documents": [], "access_token": "a", "refresh_token": "r", "expires_in": 3600,
    "email": "u@e.com", "user_id": "u1", "role": "member",
    "sql": "SELECT 1", "results": [], "row_count": 0, "path": "/d",
}


# (callable-name path, http method, expected path, args, kwargs)
CASES = [
    # auth
    ("auth.logout", "POST", "/auth/logout", (), {}),
    ("auth.refresh", "POST", "/auth/refresh", ("rtok",), {}),
    ("auth.password_reset", "POST", "/auth/password-reset", ("u@e.com",), {}),
    # sql schema (§2.12-only wire: /namespaces/{ns}/sql/schema)
    ("sql.register_schema", "POST", "/namespaces/ns/sql/schema", ("ns", {"t": []}), {}),
    ("sql.get_schema", "GET", "/namespaces/ns/sql/schema", ("ns",), {}),
    ("sql.delete_schema", "DELETE", "/namespaces/ns/sql/schema", ("ns",), {}),
    # sync
    ("sync.stop", "POST", "/sync/stop", ("ns",), {}),
    ("sync.trigger", "POST", "/sync/trigger", ("ns",), {}),
    # system
    ("system.version", "GET", "/system/version", (), {}),
    ("system.namespace_stats", "GET", "/system/namespaces/ns/stats", ("ns",), {}),
    ("system.agent_llm_config", "GET", "/system/agent_llm_config", (), {}),
    ("system.features", "GET", "/system/features", (), {}),
    # tenants
    ("tenants.list", "GET", "/tenants", (), {}),
    ("tenants.list_members", "GET", "/tenants/t1/members", ("t1",), {}),
    ("tenants.update_role", "PATCH", "/tenants/t1/members/u1", ("t1", "u1"), {"role": "admin"}),
    ("tenants.remove_member", "DELETE", "/tenants/t1/members/u1", ("t1", "u1"), {}),
    ("tenants.quota", "GET", "/tenants/t1/quota", ("t1",), {}),
    # namespaces (sync update + async path)
    ("namespaces.update", "PATCH", "/namespaces/ns", ("ns",), {"description": "d"}),
    ("namespaces.get", "GET", "/namespaces/ns", ("ns",), {}),
    # watchers
    ("watchers.events", "GET", "/watch/w1/events", ("w1",), {}),
    # documents
    ("documents.status", "GET", "/documents/d1", ("d1",), {}),
    # query top-level get_sources (D3.5 endpoint)
    ("query.get_sources", "GET", "/interactions/i1/sources", ("i1",), {}),
    # ops
    ("ops.gc.restore", "POST", "/gc/restore", (["d1"],), {}),
    ("ops.gc.purge", "POST", "/gc/purge", (), {}),
]


def _resolve(client, dotted: str):
    obj = client
    for part in dotted.split("."):
        obj = getattr(obj, part)
    return obj


def _route(method: str, path: str):
    return getattr(respx, method.lower())(API + path).mock(
        return_value=httpx.Response(200, json=_SWEEP_JSON)
    )


@pytest.mark.parametrize("name,method,path,args,kwargs", CASES)
@respx.mock
def test_sync_resource_method(name, method, path, args, kwargs) -> None:
    route = _route(method, path)
    c = Cortrix(base_url=BASE, api_key="k", max_retries=0)
    _resolve(c, name)(*args, **kwargs)
    assert route.called and route.calls.last.request.method == method
    c.close()


@pytest.mark.parametrize("name,method,path,args,kwargs", CASES)
@respx.mock
async def test_async_resource_method(name, method, path, args, kwargs) -> None:
    route = _route(method, path)
    c = AsyncCortrix(base_url=BASE, api_key="k", max_retries=0)
    await _resolve(c, name)(*args, **kwargs)
    assert route.called and route.calls.last.request.method == method
    await c.close()


# A few async-only direct calls for resources whose sync path is covered but the
# async create/list path is not.
@respx.mock
async def test_async_namespaces_create_and_list() -> None:
    respx.post(API + "/namespaces").mock(
        return_value=httpx.Response(201, json={"namespace": "n", "status": "active"})
    )
    respx.get(API + "/namespaces").mock(
        return_value=httpx.Response(200, json={"namespaces": [], "total": 0})
    )
    respx.post(API + "/namespaces/n/acl").mock(return_value=httpx.Response(201, json={}))
    c = AsyncCortrix(base_url=BASE, api_key="k")
    await c.namespaces.create("n", display_name="d", visibility="private")
    await c.namespaces.list()
    await c.namespaces.set_permission("n", "t2", permission="read")
    await c.close()


@respx.mock
async def test_async_resource_sweep() -> None:
    """Drive the remaining async resource methods (create/list/upload variants)."""
    for m, p in [
        ("post", "/documents"), ("get", "/documents"), ("delete", "/documents/d1"),
        ("get", "/documents/tasks/t1/progress"), ("delete", "/documents/tasks/t1"),
        ("post", "/sql"), ("post", "/memory"), ("get", "/memory"),
        ("post", "/memory/sessions/s1/interactions"), ("post", "/memory/extract"),
        ("post", "/watch"), ("get", "/watch"), ("delete", "/watch/w1"),
        ("post", "/sync/start"), ("get", "/sync/status"),
        ("post", "/auth/register"), ("post", "/auth/login"), ("get", "/auth/me"),
        ("get", "/tenants/t1"), ("post", "/admin/tenants"),
        ("post", "/import/database"), ("get", "/system/health/live"),
        ("get", "/gc/status"), ("post", "/gc/run"),
    ]:
        getattr(respx, m)(API + p).mock(return_value=httpx.Response(200, json=_SWEEP_JSON))
    c = AsyncCortrix(base_url=BASE, api_key="k")
    await c.documents.upload("n", io.BytesIO(b"hi"), filename="f.txt")
    await c.documents.list("n")
    await c.documents.delete("d1")
    await c.documents.task_progress("t1")
    await c.documents.cancel_task("t1")
    await c.sql.query(question="q")
    await c.memory.create("n", "c", memory_type="fact")
    await c.memory.list(namespace="n")
    await c.memory.log("n", query="q", response="r", user_id="u", session_id="s1")
    await c.watchers.add("/d", ["n"])
    await c.watchers.list()
    await c.watchers.remove("w1")
    await c.sync.configure("n")
    await c.sync.status()
    await c.auth.register("u@e.com", "p")
    await c.auth.login("u@e.com", "p")
    await c.auth.me()
    await c.tenants.get("t1")
    await c.tenants.create("x")
    await c.import_database("n", connection={"ref": "c1"})
    await c.system.health()
    await c.ops.gc.status()
    await c.ops.gc.run()
    await c.close()
