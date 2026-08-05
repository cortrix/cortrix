#pragma once

namespace httplib { class Server; }

namespace cortrix {

class ApiKeyAuth;

namespace tenant {
class TenantService;
class PermissionService;
class QuotaService;
}  // namespace tenant

/// Register the tenant/permission/quota HTTP routes (15 endpoints)
/// plus the sec 4.6 PermissionMiddleware behavior (the namespace ACL endpoints run
/// the composite CanX checks; the tenant-admin endpoints enforce the sec 4.5 admin-
/// only column via AuthContext.is_admin()).
///
/// Endpoints (all under /api/v1):
///   POST   /tenants                                   CreateOrganization (admin)
///   GET    /tenants/{id}                              Get (admin or member)
///   GET    /tenants                                   list all (admin)
///   PATCH  /tenants/{id}                              UpdateName (admin)
///   DELETE /tenants/{id}                              Delete (admin)
///   POST   /tenants/{id}/members                      AddMember (admin)
///   GET    /tenants/{id}/members                      ListMembers (admin or member)
///   DELETE /tenants/{id}/members/{user_id}            RemoveMember (admin)
///   PATCH  /tenants/{id}/members/{user_id}            UpdateMemberRole (admin)
///   POST   /tenants/{id}/transfer-ownership           TransferOwnership (admin)
///   GET    /namespaces/{ns_id}/acl                    ListAcl (owner / ACL admin)
///   POST   /namespaces/{ns_id}/acl                    GrantAcl (owner / ACL admin)
///   DELETE /namespaces/{ns_id}/acl/{grantee_tenant_id} RevokeAcl (owner / ACL admin)
///   GET    /tenants/{id}/quota                        GetQuota (member)
///   PATCH  /admin/tenants/{id}/quota                  UpdateQuotaOverride (admin)
///
/// `POST /api/v1/auth/switch-tenant` is deliberately NOT registered (topic 5 ->
/// HTTP 404 is the Agent-friendly signal; BACKLOG TD-AUTH-SWITCH-TENANT-ENDPOINT).
///
/// Shared-file rule (D-R1 sec 2): this is the single encapsulated entry point to
/// be added to src/main.cpp / the server bootstrap (one ADD line); the route
/// bodies live here, not inline. Wiring it into main.cpp requires a catalog.db
/// handle + the three services, which is cross-feature bootstrap -> D3.5.
void RegisterTenantRoutes(httplib::Server& server,
                          tenant::TenantService& tenant_svc,
                          tenant::PermissionService& perm_svc,
                          tenant::QuotaService& quota_svc,
                          ApiKeyAuth& auth);

}  // namespace cortrix
