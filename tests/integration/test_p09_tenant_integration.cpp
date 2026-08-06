// Tenancy -- standalone integration tests over a real in-memory catalog.db.
//
// The cross-feature IT cases that require auth / cross-NS query / DB import / memory isolation *live* wiring
// (IT47 cross-NS query call-site switch, IT48 DB import, IT49 memory isolation) are integration-deferred per the
// standalone-first rule; here we exercise the tenancy contracts those features rely on
// against the frozen catalog schema:
//   IT45/46 -- the auth registration cross-service transaction (auth holds the tx,
//             Tenancy CreatePersonal runs on the SAME conn; COMMIT persists all,
//             ROLLBACK drops all -- users + tenants + user_tenants atomic).
//   IT47    -- cross-NS query BatchCheck call pattern end-to-end.
//   IT50/51 -- admin HTTP quota / tenant-create service paths.
//   IT53    -- switch-tenant is unregistered (404 contract) -- asserted at the
//             route layer (documented; the route is intentionally absent).
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <sqlite3.h>

#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/tenant/i_plan_provider.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/quota_service.h"
#include "cortrix/tenant/tenant_error.h"
#include "cortrix/tenant/tenant_service.h"

namespace cortrix::tenant {
namespace {

using catalog::CatalogDb;

void SeedUser(sqlite3* db, const std::string& uid, const std::string& email) {
    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT INTO users(user_id, email, created_at) VALUES(?,?,0)",
                                 -1, &s, nullptr), SQLITE_OK);
    sqlite3_bind_text(s, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(s), SQLITE_DONE);
    sqlite3_finalize(s);
}

int CountRows(sqlite3* db, const char* table) {
    std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return -1;
    int n = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}

class P09IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        tenant_svc_ = std::make_unique<TenantService>(catalog_.db());
        perm_svc_ = std::make_unique<PermissionService>(catalog_.db());
        quota_svc_ = std::make_unique<QuotaService>(std::make_shared<UnlimitedPlanProvider>(),
                                                    catalog_.db());
        // The tenancy bootstrap seed (schema-embedded) pre-populates users /
        // tenants / user_tenants on Open(); register-flow assertions are relative
        // to this baseline, not to zero.
        users_base_ = CountRows(catalog_.db(), "users");
        tenants_base_ = CountRows(catalog_.db(), "tenants");
        memberships_base_ = CountRows(catalog_.db(), "user_tenants");
    }
    CatalogDb catalog_;
    std::unique_ptr<TenantService> tenant_svc_;
    std::unique_ptr<PermissionService> perm_svc_;
    std::unique_ptr<QuotaService> quota_svc_;
    int users_base_ = 0;
    int tenants_base_ = 0;
    int memberships_base_ = 0;
};

// --- IT45: registration flow commits users + tenants + user_tenants together ---
TEST_F(P09IntegrationTest, Register_Flow_CommitsAllThreeTables) {
    sqlite3* db = catalog_.db();
    // Auth holds the outermost transaction.
    ASSERT_EQ(sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
    SeedUser(db, "u1", "new@x.com");  // Auth step 3: INSERT users (on this conn)
    auto r = tenant_svc_->CreatePersonal("u1", "new@x.com", /*conn=*/db);  // Auth step 4
    ASSERT_TRUE(r.ok()) << r.status().message();
    ASSERT_EQ(sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);

    EXPECT_EQ(CountRows(db, "users"), users_base_ + 1);
    EXPECT_EQ(CountRows(db, "tenants"), tenants_base_ + 1);
    EXPECT_EQ(CountRows(db, "user_tenants"), memberships_base_ + 1);
}

// --- IT46: a registration rollback drops all three tables' rows atomically ---
TEST_F(P09IntegrationTest, Register_Flow_Failure_RollbackAll) {
    sqlite3* db = catalog_.db();
    ASSERT_EQ(sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
    SeedUser(db, "u1", "new@x.com");
    auto r = tenant_svc_->CreatePersonal("u1", "new@x.com", db);
    ASSERT_TRUE(r.ok());
    // Auth hits a later failure (e.g. JWT issue) and rolls the whole tx back.
    ASSERT_EQ(sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);

    EXPECT_EQ(CountRows(db, "users"), users_base_);
    EXPECT_EQ(CountRows(db, "tenants"), tenants_base_);
    EXPECT_EQ(CountRows(db, "user_tenants"), memberships_base_);
}

// --- IT47: cross-NS query BatchCheck over a mix of owned / granted / denied namespaces ---
TEST_F(P09IntegrationTest, F04_CrossNSQuery_BatchCheck) {
    sqlite3* db = catalog_.db();
    ASSERT_EQ(sqlite3_exec(db,
                           "INSERT INTO tenants(tenant_id,type,name,created_at) VALUES('t1','personal','t1',0);"
                           "INSERT INTO tenants(tenant_id,type,name,created_at) VALUES('t2','personal','t2',0);"
                           "INSERT INTO namespaces(ns_id,name,tenant_id,visibility,created_at) VALUES('own','own','t1','tenant',0);"
                           "INSERT INTO namespaces(ns_id,name,tenant_id,visibility,created_at) VALUES('granted','granted','t2','tenant',0);"
                           "INSERT INTO namespaces(ns_id,name,tenant_id,visibility,created_at) VALUES('denied','denied','t2','tenant',0);"
                           "INSERT INTO ns_acl(ns_id,grantee_tenant_id,grantee_user_id,role,granted_at) VALUES('granted','t1',NULL,'viewer',0);",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    AuthContext ctx;
    ctx.tenant_id = "t1";
    ctx.user_id = "u1";
    auto res = perm_svc_->BatchCheck(ctx, {"own", "granted", "denied"});
    ASSERT_EQ(res.size(), 3u);
    EXPECT_TRUE(res[0].can_read);   // owned
    EXPECT_TRUE(res[0].can_write);
    EXPECT_TRUE(res[1].can_read);   // granted viewer
    EXPECT_FALSE(res[1].can_write); // viewer can't write
    EXPECT_FALSE(res[2].can_read);  // denied
}

// --- IT50: admin HTTP quota check path -> UnlimitedPlanProvider returns unlimited ---
TEST_F(P09IntegrationTest, Admin_HttpQuotaCheck_V1OSS) {
    sqlite3* db = catalog_.db();
    // OR IGNORE: the tenancy bootstrap seed already inserts default_tenant.
    ASSERT_EQ(sqlite3_exec(db,
                           "INSERT OR IGNORE INTO tenants(tenant_id,type,name,created_at) VALUES('default_tenant','personal','Default',0)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    AuthContext admin;
    admin.user_id = "admin";
    admin.permissions = kPermAdmin;
    auto r = quota_svc_->GetQuota("default_tenant", admin);
    ASSERT_TRUE(r.ok()) << r.status().message();
    for (const auto& [type, lim] : r->limits) EXPECT_TRUE(lim.IsUnlimited());
}

// --- IT51: admin tenant-create succeeds end-to-end ---
TEST_F(P09IntegrationTest, AdminApiKey_TenantCreate_Success) {
    SeedUser(catalog_.db(), "admin", "admin@x.com");
    AuthContext admin;
    admin.user_id = "admin";
    admin.permissions = kPermAdmin;
    auto r = tenant_svc_->CreateOrganization("Acme Corp", admin);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r->type, TenantType::ORGANIZATION);
    // The creator is recorded as owner.
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(catalog_.db(),
                       "SELECT role FROM user_tenants WHERE tenant_id=? AND user_id='admin'", -1,
                       &s, nullptr);
    sqlite3_bind_text(s, 1, r->tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)), "owner");
    sqlite3_finalize(s);
}

// --- IT (ACL roundtrip across tenants) -- owner grants cross-tenant access ---
TEST_F(P09IntegrationTest, CrossTenant_AclGrant_EnablesRead) {
    sqlite3* db = catalog_.db();
    ASSERT_EQ(sqlite3_exec(db,
                           "INSERT INTO tenants(tenant_id,type,name,created_at) VALUES('owner-t','personal','o',0);"
                           "INSERT INTO tenants(tenant_id,type,name,created_at) VALUES('guest-t','personal','g',0);"
                           "INSERT INTO namespaces(ns_id,name,tenant_id,visibility,created_at) VALUES('shared','shared','owner-t','tenant',0);",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    AuthContext owner;
    owner.tenant_id = "owner-t";
    owner.user_id = "owner";
    AuthContext guest;
    guest.tenant_id = "guest-t";
    guest.user_id = "guest";

    EXPECT_FALSE(perm_svc_->CanRead(guest, "shared"));  // before grant
    ASSERT_TRUE(perm_svc_->GrantAcl("shared", "guest-t", "", NsAclRole::VIEWER, owner).ok());
    EXPECT_TRUE(perm_svc_->CanRead(guest, "shared"));   // after grant
    EXPECT_FALSE(perm_svc_->CanWrite(guest, "shared")); // viewer read-only
}

}  // namespace
}  // namespace cortrix::tenant
