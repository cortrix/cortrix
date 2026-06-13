"""Namespaces resource tests (/namespaces domain, real-arch wire)."""

from __future__ import annotations

import json

import httpx
import respx

from cortrix import Cortrix
from cortrix.types import Namespace, NamespaceList


@respx.mock
def test_create_basic(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/namespaces").mock(
        return_value=httpx.Response(
            201, json={"namespace": "contracts", "status": "active", "display_name": "Contracts library"}
        )
    )
    ns = client.namespaces.create("contracts", display_name="Contracts library", visibility="private")
    assert isinstance(ns, Namespace) and ns.namespace == "contracts"
    body = json.loads(route.calls.last.request.content)
    assert body == {"name": "contracts", "display_name": "Contracts library", "visibility": "private"}


@respx.mock
def test_list(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/namespaces").mock(
        return_value=httpx.Response(
            200,
            json={"namespaces": [{"namespace": "a", "status": "active"},
                                 {"namespace": "b", "status": "active"}], "total": 2},
        )
    )
    out = client.namespaces.list()
    assert isinstance(out, NamespaceList) and len(out) == 2
    assert [n.namespace for n in out] == ["a", "b"]


@respx.mock
def test_get(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/namespaces/contracts").mock(
        return_value=httpx.Response(200, json={"namespace": "contracts", "status": "active"})
    )
    assert client.namespaces.get("contracts").namespace == "contracts"


@respx.mock
def test_update_patch(api_base: str, client: Cortrix) -> None:
    route = respx.patch(api_base + "/namespaces/contracts").mock(
        return_value=httpx.Response(200, json={"namespace": "contracts", "status": "active"})
    )
    client.namespaces.update("contracts", display_name="New name")
    assert route.calls.last.request.method == "PATCH"
    assert json.loads(route.calls.last.request.content) == {"display_name": "New name"}


@respx.mock
def test_delete(api_base: str, client: Cortrix) -> None:
    route = respx.delete(api_base + "/namespaces/contracts").mock(
        return_value=httpx.Response(202, json={"namespace": "contracts", "status": "deleting"})
    )
    client.namespaces.delete("contracts")
    assert route.calls.last.request.method == "DELETE"


@respx.mock
def test_set_permission_acl_grant(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/namespaces/contracts/acl").mock(
        return_value=httpx.Response(201, json={"grantee_tenant_id": "t-2", "permission": "read"})
    )
    client.namespaces.set_permission("contracts", "t-2", permission="read")
    body = json.loads(route.calls.last.request.content)
    assert body == {"grantee_tenant_id": "t-2", "permission": "read"}
