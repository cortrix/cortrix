// P09 sec 11.1 -- PermissionMiddleware behavior (UT41-42).
//
// The sec 4.6 PermissionMiddleware is realized in tenant_routes.cpp as the per-route
// CanX gating over PermissionService (one code path, data-state controlled -- topic 3).
// These tests pin the two middleware contracts that do not require an HTTP server:
//   UT41 -- V1.0 OSS single default_tenant: owner match -> CanRead/CanWrite pass
//          with NO ns_acl rows present (the 0-extra-IO hot path).
//   UT42 -- the CX_ERR_NS_UNAUTHORIZED body carries the full sec 7 structured_data
//          (ns_id / tenant_id / user_id / required_action) the middleware emits.
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/tenant_error.h"

namespace cortrix::tenant {
namespace {

using catalog::CatalogDb;

// --- UT41: V1.0 OSS default_tenant naturally passes (owns its NS, no ACL rows) ---
TEST(PermissionMiddlewareTest, DefaultTenant_Pass) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    // OR IGNORE: the P09 §10.1 bootstrap seed (schema-embedded) already inserts
    // default_tenant on Open(); this just guarantees the row for the test.
    ASSERT_EQ(sqlite3_exec(catalog.db(),
                           "INSERT OR IGNORE INTO tenants(tenant_id, type, name, created_at) "
                           "VALUES('default_tenant','personal','Default Tenant',0)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(catalog.db(),
                           "INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
                           "VALUES('ns1','ns1','default_tenant','tenant',0)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    PermissionService svc(catalog.db());

    AuthContext ctx;
    ctx.tenant_id = "default_tenant";
    ctx.user_id = "default_user";
    ctx.permissions = kPermRead | kPermWrite | kPermAdmin;

    // No ns_acl rows exist; owner match alone must allow read + write + admin + delete.
    EXPECT_TRUE(svc.CanRead(ctx, "ns1"));
    EXPECT_TRUE(svc.CanWrite(ctx, "ns1"));
    EXPECT_TRUE(svc.CanAdminAcl(ctx, "ns1"));
    EXPECT_TRUE(svc.CanDelete(ctx, "ns1"));

    // Sanity: the ns_acl table is empty (the pass did not depend on a grant).
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(catalog.db(), "SELECT COUNT(*) FROM ns_acl", -1, &s, nullptr);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(s, 0), 0);
    sqlite3_finalize(s);
}

// --- UT42: the unauthorized error body is structured + complete ---
TEST(PermissionMiddlewareTest, NsUnauthorized_StructuredError) {
    nlohmann::json sd = {
        {"ns_id", "finance-ns"},
        {"tenant_id", "tenant-abc"},
        {"user_id", "user-xyz"},
        {"required_action", "read"},
    };
    ASSERT_TRUE(HasRequiredStructuredData(TenantErrorCode::kNsUnauthorized, sd));

    auto err = MakeTenantError(
        TenantErrorCode::kNsUnauthorized, sd,
        "User does not have read permission on namespace 'finance-ns'");
    auto body = agent_friendly::ToJson(err);

    EXPECT_EQ(body["code"], "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_EQ(body["category"], "auth");
    EXPECT_EQ(body["retryable"], false);
    EXPECT_TRUE(body["retry_after_ms"].is_null());
    ASSERT_TRUE(body.contains("structured_data"));
    EXPECT_EQ(body["structured_data"]["ns_id"], "finance-ns");
    EXPECT_EQ(body["structured_data"]["tenant_id"], "tenant-abc");
    EXPECT_EQ(body["structured_data"]["user_id"], "user-xyz");
    EXPECT_EQ(body["structured_data"]["required_action"], "read");
}

}  // namespace
}  // namespace cortrix::tenant
