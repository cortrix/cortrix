"""Tenants resource. ``/tenants/*`` (user-level, P09) + ``/admin/tenants``.

Per Derek's ruling (briefing § 9): SDK shape = P03 § 2.12; wire = real
architecture. Mapping:
  - ``list``         -> GET    /tenants                    (listMyTenants)
  - ``get``          -> GET    /tenants/{id}
  - ``list_members`` -> GET    /tenants/{id}/members
  - ``invite``       -> POST   /tenants/{id}/members        (addTenantMember)
  - ``update_role``  -> PATCH  /tenants/{id}/members/{uid}  (updateTenantMember)
  - ``remove_member``-> DELETE /tenants/{id}/members/{uid}
  - ``quota``        -> GET    /tenants/{id}/quota
  - ``create``       -> POST   /admin/tenants  (real-arch creation is admin-scope;
                        § 2.12's user-level POST /tenants reconciled in D3.5)
"""

from __future__ import annotations

from typing import Any, Optional

from ..types import Quota, Tenant, TenantMember
from ._base import AsyncResource, SyncResource

PATH_TENANTS = "/tenants"
PATH_TENANT = "/tenants/{id}"
PATH_MEMBERS = "/tenants/{id}/members"
PATH_MEMBER = "/tenants/{id}/members/{uid}"
PATH_QUOTA = "/tenants/{id}/quota"
PATH_ADMIN_TENANTS = "/admin/tenants"  # creation is admin-scope


class Tenants(SyncResource):
    """Tenant management. ``/tenants/*`` (user) + ``/admin/tenants``."""

    def list(self) -> Any:
        """List my tenants. ``GET /tenants``."""
        return self._client._request("GET", PATH_TENANTS)

    def get(self, tenant_id: str) -> Tenant:
        """Get tenant. ``GET /tenants/{id}`` -> Tenant."""
        return self._client._request(
            "GET", PATH_TENANT.format(id=tenant_id), response_model=Tenant
        )

    def list_members(self, tenant_id: str) -> Any:
        """List members. ``GET /tenants/{id}/members``."""
        return self._client._request("GET", PATH_MEMBERS.format(id=tenant_id))

    def invite(self, tenant_id: str, *, email: str, role: str = "member") -> TenantMember:
        """Invite/add a member. ``POST /tenants/{id}/members`` -> TenantMember."""
        return self._client._request(
            "POST", PATH_MEMBERS.format(id=tenant_id), json={"email": email, "role": role},
            response_model=TenantMember,
        )

    def update_role(self, tenant_id: str, user_id: str, *, role: str) -> TenantMember:
        """Update a member's role. ``PATCH /tenants/{id}/members/{uid}`` -> TenantMember."""
        return self._client._request(
            "PATCH", PATH_MEMBER.format(id=tenant_id, uid=user_id), json={"role": role},
            response_model=TenantMember,
        )

    def remove_member(self, tenant_id: str, user_id: str) -> None:
        """Remove a member. ``DELETE /tenants/{id}/members/{uid}``."""
        self._client._request("DELETE", PATH_MEMBER.format(id=tenant_id, uid=user_id))

    def quota(self, tenant_id: str) -> Quota:
        """Get tenant quota. ``GET /tenants/{id}/quota`` -> Quota."""
        return self._client._request(
            "GET", PATH_QUOTA.format(id=tenant_id), response_model=Quota
        )

    def create(self, name: str, *, owner_user_id: Optional[str] = None) -> Tenant:
        """Create a tenant (**admin-scope** in spec). ``POST /admin/tenants`` -> Tenant."""
        body: dict[str, Any] = {"name": name}
        if owner_user_id is not None:
            body["owner_user_id"] = owner_user_id
        return self._client._request(
            "POST", PATH_ADMIN_TENANTS, json=body, response_model=Tenant
        )


class AsyncTenants(AsyncResource):
    """Async Tenants (symmetric to :class:`Tenants`)."""

    async def list(self) -> Any:
        return await self._client._request("GET", PATH_TENANTS)

    async def get(self, tenant_id: str) -> Tenant:
        return await self._client._request(
            "GET", PATH_TENANT.format(id=tenant_id), response_model=Tenant
        )

    async def list_members(self, tenant_id: str) -> Any:
        return await self._client._request("GET", PATH_MEMBERS.format(id=tenant_id))

    async def invite(
        self, tenant_id: str, *, email: str, role: str = "member"
    ) -> TenantMember:
        return await self._client._request(
            "POST", PATH_MEMBERS.format(id=tenant_id), json={"email": email, "role": role},
            response_model=TenantMember,
        )

    async def update_role(self, tenant_id: str, user_id: str, *, role: str) -> TenantMember:
        return await self._client._request(
            "PATCH", PATH_MEMBER.format(id=tenant_id, uid=user_id), json={"role": role},
            response_model=TenantMember,
        )

    async def remove_member(self, tenant_id: str, user_id: str) -> None:
        await self._client._request("DELETE", PATH_MEMBER.format(id=tenant_id, uid=user_id))

    async def quota(self, tenant_id: str) -> Quota:
        return await self._client._request(
            "GET", PATH_QUOTA.format(id=tenant_id), response_model=Quota
        )

    async def create(self, name: str, *, owner_user_id: Optional[str] = None) -> Tenant:
        body: dict[str, Any] = {"name": name}
        if owner_user_id is not None:
            body["owner_user_id"] = owner_user_id
        return await self._client._request(
            "POST", PATH_ADMIN_TENANTS, json=body, response_model=Tenant
        )
