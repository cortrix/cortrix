#pragma once
// Test helper: wire ApiKeyAuth's runtime namespace-authorization seams
// (ARCHITECTURE V6) to a REAL cortrix::tenant::PermissionService backed by an
// in-memory catalog seeded with the namespaces owned by a test tenant.
//
// Mirrors production bootstrap wiring so route/integration/security tests
// exercise the genuine authorization path (ownership + ns_acl via BatchCheck)
// instead of the removed static per-key allow-list. Hold an instance as a
// fixture member: it owns the catalog + PermissionService that the installed
// seam closures capture by raw pointer.
//
// Usage:
//   NamespaceAuthzHarness authz_{auth_, &pool_, "test-tenant", {"ns-a", "ns-b"}};
//   // -> a key whose tenant_id == "test-tenant" can now access ns-a / ns-b.

#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/tenant/permission_service.h"

namespace cortrix::test {

class NamespaceAuthzHarness {
public:
    /// `owned` namespaces are seeded as owned by `tenant`; a principal whose
    /// AuthContext.tenant_id == `tenant` is then authorized for them (CheckOwnerTenant).
    /// `pool` may be null for tests that only exercise the per-namespace gate
    /// (no GET /namespaces list endpoint).
    NamespaceAuthzHarness(ApiKeyAuth& auth, resource::INamespacePool* pool,
                          const std::string& tenant,
                          const std::vector<std::string>& owned) {
        catalog_.Open(":memory:");
        for (const auto& ns : owned) Seed(ns, tenant);
        perm_ = std::make_unique<tenant::PermissionService>(catalog_.db());
        Wire(auth, pool);
    }

    /// Seed an additional namespace owned by `tenant` after construction.
    void AddOwned(const std::string& ns, const std::string& tenant) { Seed(ns, tenant); }

    tenant::PermissionService& service() { return *perm_; }
    sqlite3* catalog_db() { return catalog_.db(); }

private:
    void Seed(const std::string& ns, const std::string& tenant) {
        const std::string sql =
            "INSERT OR IGNORE INTO tenants(tenant_id,type,name,created_at) "
            "VALUES('" + tenant + "','personal','t',0);"
            "INSERT OR REPLACE INTO namespaces(ns_id,name,tenant_id,visibility,created_at) "
            "VALUES('" + ns + "','" + ns + "','" + tenant + "','private',0);";
        sqlite3_exec(catalog_.db(), sql.c_str(), nullptr, nullptr, nullptr);
    }

    void Wire(ApiKeyAuth& auth, resource::INamespacePool* pool) {
        tenant::PermissionService* svc = perm_.get();
        auth.SetNamespaceAuthorizer(
            [svc](const AuthContext& ctx, const std::string& ns) -> bool {
                auto checks = svc->BatchCheck(ctx, {ns});
                return !checks.empty() && checks.front().can_read;
            });
        if (pool != nullptr) {
            auth.SetNamespaceLister(
                [svc, pool](const AuthContext& ctx) -> std::vector<std::string> {
                    std::vector<std::string> active = pool->GetExplainState().active_namespaces;
                    if (active.empty()) return active;
                    auto checks = svc->BatchCheck(ctx, active);
                    std::vector<std::string> out;
                    out.reserve(checks.size());
                    for (const auto& c : checks) {
                        if (c.can_read) out.push_back(c.ns_id);
                    }
                    return out;
                });
        }
    }

    catalog::CatalogDb catalog_;
    std::unique_ptr<tenant::PermissionService> perm_;
};

}  // namespace cortrix::test
