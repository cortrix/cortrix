"""Extended resource tests: watchers / sync / auth / system / tenants / import."""

from __future__ import annotations

import json

import httpx
import respx

from cortrix import Cortrix
from cortrix.types import LoginResponse, SyncStatus, Tenant, User, Watcher


# --- watchers ---
@respx.mock
def test_watchers_add(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/watch").mock(
        return_value=httpx.Response(201, json={"id": "w1", "path": "/data", "status": "active"})
    )
    w = client.watchers.add("/data", ["team_a", "team_b"], recursive=True)
    assert isinstance(w, Watcher) and w.id == "w1"
    body = json.loads(route.calls.last.request.content)
    assert body == {"path": "/data", "target_namespaces": ["team_a", "team_b"], "recursive": True}


@respx.mock
def test_watchers_list_and_remove(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/watch").mock(
        return_value=httpx.Response(200, json={"watchers": [{"id": "w1", "path": "/d"}]})
    )
    assert len(client.watchers.list()) == 1
    route = respx.delete(api_base + "/watch/w1").mock(return_value=httpx.Response(204))
    assert client.watchers.remove("w1") is None
    assert route.calls.last.request.method == "DELETE"


# --- sync ---
@respx.mock
def test_sync_configure_maps_to_start(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/sync/start").mock(
        return_value=httpx.Response(200, json={"status": "running"})
    )
    s = client.sync.configure("ns", interval_seconds=600)
    assert isinstance(s, SyncStatus) and s.status == "running"
    assert json.loads(route.calls.last.request.content) == {"namespace": "ns", "interval_seconds": 600}


@respx.mock
def test_sync_status_optional_namespace(api_base: str, client: Cortrix) -> None:
    route = respx.get(api_base + "/sync/status").mock(
        return_value=httpx.Response(200, json={"status": "idle"})
    )
    client.sync.status()
    assert "namespace" not in route.calls.last.request.url.params
    client.sync.status("ns")
    assert route.calls.last.request.url.params["namespace"] == "ns"


# --- auth ---
@respx.mock
def test_auth_login(api_base: str, client: Cortrix) -> None:
    respx.post(api_base + "/auth/login").mock(
        return_value=httpx.Response(
            200, json={"access_token": "a", "refresh_token": "r", "expires_in": 3600}
        )
    )
    lr = client.auth.login("u@e.com", "pw")
    assert isinstance(lr, LoginResponse) and lr.access_token == "a"


@respx.mock
def test_auth_register_and_me(api_base: str, client: Cortrix) -> None:
    respx.post(api_base + "/auth/register").mock(
        return_value=httpx.Response(201, json={"id": "u1", "email": "u@e.com"})
    )
    u = client.auth.register("u@e.com", "pw", display_name="Zhang San")
    assert isinstance(u, User) and u.id == "u1"
    respx.get(api_base + "/auth/me").mock(
        return_value=httpx.Response(200, json={"id": "u1", "email": "u@e.com"})
    )
    assert client.auth.me().id == "u1"


# --- system ---
@respx.mock
def test_system_health(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/system/health/live").mock(
        return_value=httpx.Response(200, json={"status": "alive", "version": "1.0.0-rc.1"})
    )
    h = client.system.health()
    assert h["status"] == "alive"  # raw dict (no dedicated model in spec)


# --- tenants ---
@respx.mock
def test_tenants_get_and_invite(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/tenants/t1").mock(
        return_value=httpx.Response(200, json={"tenant_id": "t1", "name": "Acme"})
    )
    assert isinstance(client.tenants.get("t1"), Tenant)
    route = respx.post(api_base + "/tenants/t1/members").mock(
        return_value=httpx.Response(201, json={"user_id": "u2", "role": "member"})
    )
    client.tenants.invite("t1", email="x@e.com", role="member")
    assert json.loads(route.calls.last.request.content) == {"email": "x@e.com", "role": "member"}


@respx.mock
def test_tenants_create_uses_admin_endpoint(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/admin/tenants").mock(
        return_value=httpx.Response(201, json={"tenant_id": "t9", "name": "New"})
    )
    client.tenants.create("New")
    assert route.calls.last.request.url.path == "/api/v1/admin/tenants"


# --- import (DB import hand-written, mocked; formal spec to follow) ---
@respx.mock
def test_import_database(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/import/database").mock(
        return_value=httpx.Response(202, json={"task_id": "imp_1", "status": "queued"})
    )
    client.import_database(
        "ns", connection={"ref": "conn_1"}, query="SELECT * FROM t", mode="merge"
    )
    body = json.loads(route.calls.last.request.content)
    assert body["namespace"] == "ns" and body["mode"] == "merge"
    assert body["connection"] == {"ref": "conn_1"} and body["query"] == "SELECT * FROM t"
