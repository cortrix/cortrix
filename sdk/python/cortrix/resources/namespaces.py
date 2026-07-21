"""Namespaces resource. ``/namespaces`` domain (ARCH § 4.1.1).

This resource follows the implemented HTTP architecture. ``set_permission()`` uses the ACL domain
(``POST /namespaces/{ns}/acl`` grant) — § 2.12's ``PUT
/namespaces/:ns/permissions/:uid`` is the obsoleted D1 draft.
"""

from __future__ import annotations

from typing import Any, Optional

from ..types import Namespace
from ..types.lists import NamespaceList
from ._base import AsyncResource, SyncResource

PATH_NAMESPACES = "/namespaces"
PATH_NAMESPACE = "/namespaces/{ns}"
PATH_NS_ACL = "/namespaces/{ns}/acl"


def _create_body(
    name: str,
    display_name: Optional[str],
    description: Optional[str],
    embedding_model: Optional[str],
    chunk_strategy: Optional[dict[str, Any]],
    visibility: Optional[str],
) -> dict[str, Any]:
    body: dict[str, Any] = {"name": name}
    if display_name is not None:
        body["display_name"] = display_name
    if description is not None:
        body["description"] = description
    if embedding_model is not None:
        body["embedding_model"] = embedding_model
    if chunk_strategy is not None:
        body["chunk_strategy"] = chunk_strategy
    if visibility is not None:
        body["visibility"] = visibility
    return body


class Namespaces(SyncResource):
    """Namespace management. ``/namespaces`` domain."""

    def create(
        self,
        name: str,
        *,
        display_name: Optional[str] = None,
        description: Optional[str] = None,
        embedding_model: Optional[str] = None,
        chunk_strategy: Optional[dict[str, Any]] = None,
        visibility: Optional[str] = None,
    ) -> Namespace:
        """Create a namespace. ``POST /namespaces`` -> 201 Namespace."""
        return self._client._request(
            "POST",
            PATH_NAMESPACES,
            json=_create_body(
                name, display_name, description, embedding_model, chunk_strategy, visibility
            ),
            response_model=Namespace,
        )

    def list(self, *, limit: int = 50, offset: int = 0) -> NamespaceList:
        """List authorized namespaces. ``GET /namespaces``."""
        return self._client._request(
            "GET",
            PATH_NAMESPACES,
            params={"limit": limit, "offset": offset},
            response_model=NamespaceList,
        )

    def get(self, name: str) -> Namespace:
        """Get a namespace. ``GET /namespaces/{ns}``."""
        return self._client._request(
            "GET", PATH_NAMESPACE.format(ns=name), response_model=Namespace
        )

    def update(
        self,
        name: str,
        *,
        display_name: Optional[str] = None,
        description: Optional[str] = None,
        chunk_strategy: Optional[dict[str, Any]] = None,
        visibility: Optional[str] = None,
    ) -> Namespace:
        """Update namespace config. ``PATCH /namespaces/{ns}``."""
        body: dict[str, Any] = {}
        if display_name is not None:
            body["display_name"] = display_name
        if description is not None:
            body["description"] = description
        if chunk_strategy is not None:
            body["chunk_strategy"] = chunk_strategy
        if visibility is not None:
            body["visibility"] = visibility
        return self._client._request(
            "PATCH", PATH_NAMESPACE.format(ns=name), json=body, response_model=Namespace
        )

    def delete(self, name: str) -> Any:
        """Delete a namespace and all data. ``DELETE /namespaces/{ns}`` -> 202."""
        return self._client._request("DELETE", PATH_NAMESPACE.format(ns=name))

    def set_permission(self, name: str, grantee_tenant_id: str, *, permission: str) -> Any:
        """Grant NS ACL. ``POST /namespaces/{ns}/acl`` (spec ACL domain)."""
        return self._client._request(
            "POST",
            PATH_NS_ACL.format(ns=name),
            json={"grantee_tenant_id": grantee_tenant_id, "permission": permission},
        )


class AsyncNamespaces(AsyncResource):
    """Async Namespaces (symmetric to :class:`Namespaces`)."""

    async def create(
        self,
        name: str,
        *,
        display_name: Optional[str] = None,
        description: Optional[str] = None,
        embedding_model: Optional[str] = None,
        chunk_strategy: Optional[dict[str, Any]] = None,
        visibility: Optional[str] = None,
    ) -> Namespace:
        return await self._client._request(
            "POST",
            PATH_NAMESPACES,
            json=_create_body(
                name, display_name, description, embedding_model, chunk_strategy, visibility
            ),
            response_model=Namespace,
        )

    async def list(self, *, limit: int = 50, offset: int = 0) -> NamespaceList:
        return await self._client._request(
            "GET",
            PATH_NAMESPACES,
            params={"limit": limit, "offset": offset},
            response_model=NamespaceList,
        )

    async def get(self, name: str) -> Namespace:
        return await self._client._request(
            "GET", PATH_NAMESPACE.format(ns=name), response_model=Namespace
        )

    async def update(
        self,
        name: str,
        *,
        display_name: Optional[str] = None,
        description: Optional[str] = None,
        chunk_strategy: Optional[dict[str, Any]] = None,
        visibility: Optional[str] = None,
    ) -> Namespace:
        body: dict[str, Any] = {}
        if display_name is not None:
            body["display_name"] = display_name
        if description is not None:
            body["description"] = description
        if chunk_strategy is not None:
            body["chunk_strategy"] = chunk_strategy
        if visibility is not None:
            body["visibility"] = visibility
        return await self._client._request(
            "PATCH", PATH_NAMESPACE.format(ns=name), json=body, response_model=Namespace
        )

    async def delete(self, name: str) -> Any:
        return await self._client._request("DELETE", PATH_NAMESPACE.format(ns=name))

    async def set_permission(
        self, name: str, grantee_tenant_id: str, *, permission: str
    ) -> Any:
        return await self._client._request(
            "POST",
            PATH_NS_ACL.format(ns=name),
            json={"grantee_tenant_id": grantee_tenant_id, "permission": permission},
        )
