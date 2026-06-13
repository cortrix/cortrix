// P09 sec 11.1 -- PermissionService unit tests (UT19-35).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/tenant_error.h"

namespace cortrix::tenant {
namespace {

using catalog::CatalogDb;

bool HasCx(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;
}

AuthContext Ctx(const std::string& user_id, const std::string& tenant_id) {
    AuthContext c;
    c.user_id = user_id;
    c.tenant_id = tenant_id;
    c.permissions = kPermRead | kPermWrite;
    return c;
}

class PermissionServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) VALUES('t-owner','personal','o',0)");
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) VALUES('t-other','personal','x',0)");
        // ns1 owned by t-owner, default visibility 'tenant'.
        Exec("INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
             "VALUES('ns1','ns1','t-owner','tenant',0)");
        // ns-pub owned by t-owner, public visibility.
        Exec("INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
             "VALUES('ns-pub','ns-pub','t-owner','public',0)");
        svc_ = std::make_unique<PermissionService>(catalog_.db());
    }

    void Exec(const char* sql) {
        ASSERT_EQ(sqlite3_exec(catalog_.db(), sql, nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(catalog_.db());
    }

    CatalogDb catalog_;
    std::unique_ptr<PermissionService> svc_;
};

// --- UT19 ---
TEST_F(PermissionServiceTest, CheckOwnerTenant_Match) {
    EXPECT_TRUE(svc_->CheckOwnerTenant("ns1", "t-owner"));
    EXPECT_FALSE(svc_->CheckOwnerTenant("ns1", "t-other"));
    EXPECT_FALSE(svc_->CheckOwnerTenant("ns-missing", "t-owner"));
}

// --- UT20 / UT21: user-level grant wins over tenant-level; fallback to tenant ---
TEST_F(PermissionServiceTest, GetAclRole_UserLevelWinsThenTenantFallback) {
    // tenant-level grant: viewer for t-other (grantee_user_id NULL).
    Exec("INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at) "
         "VALUES('ns1','t-other',NULL,'viewer',0)");
    // user-level grant: editor for (t-other, alice).
    Exec("INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at) "
         "VALUES('ns1','t-other','alice','editor',0)");

    auto alice = svc_->GetAclRole("ns1", "t-other", "alice");
    ASSERT_TRUE(alice.has_value());
    EXPECT_EQ(*alice, NsAclRole::EDITOR);  // user-level wins

    auto bob = svc_->GetAclRole("ns1", "t-other", "bob");  // no user row -> tenant fallback
    ASSERT_TRUE(bob.has_value());
    EXPECT_EQ(*bob, NsAclRole::VIEWER);

    // tenant with no grant at all -> nullopt.
    EXPECT_FALSE(svc_->GetAclRole("ns1", "t-nobody", "x").has_value());
}

// --- UT23: owner always reads ---
TEST_F(PermissionServiceTest, CanRead_Owner) {
    EXPECT_TRUE(svc_->CanRead(Ctx("owner", "t-owner"), "ns1"));
}

// --- UT24: grantee viewer reads ---
TEST_F(PermissionServiceTest, CanRead_AclViewer) {
    Exec("INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at) "
         "VALUES('ns1','t-other',NULL,'viewer',0)");
    EXPECT_TRUE(svc_->CanRead(Ctx("u", "t-other"), "ns1"));
}

// --- UT25: public visibility reads even without ACL ---
TEST_F(PermissionServiceTest, CanRead_PublicVisibility) {
    EXPECT_TRUE(svc_->CanRead(Ctx("u", "t-other"), "ns-pub"));
}

// --- UT26: non-owner non-grantee non-public denied ---
TEST_F(PermissionServiceTest, CanRead_DeniedNotAcl) {
    EXPECT_FALSE(svc_->CanRead(Ctx("u", "t-other"), "ns1"));
}

// --- UT27: public NS + no ACL -> write false (public is read-only) ---
TEST_F(PermissionServiceTest, CanWrite_PublicNoAcl_False) {
    EXPECT_FALSE(svc_->CanWrite(Ctx("u", "t-other"), "ns-pub"));
}

TEST_F(PermissionServiceTest, CanWrite_OwnerAndEditor) {
    EXPECT_TRUE(svc_->CanWrite(Ctx("owner", "t-owner"), "ns1"));  // owner writes
    Exec("INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at) "
         "VALUES('ns1','t-other',NULL,'editor',0)");
    EXPECT_TRUE(svc_->CanWrite(Ctx("u", "t-other"), "ns1"));  // editor writes
}

TEST_F(PermissionServiceTest, CanWrite_ViewerCannot) {
    Exec("INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at) "
         "VALUES('ns1','t-other',NULL,'viewer',0)");
    EXPECT_FALSE(svc_->CanWrite(Ctx("u", "t-other"), "ns1"));
}

// --- UT28/29/30: owner-only ops -- ACL admin grantee cannot ---
TEST_F(PermissionServiceTest, CanDelete_OwnerOnly) {
    Exec("INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at) "
         "VALUES('ns1','t-other',NULL,'admin',0)");
    EXPECT_TRUE(svc_->CanDelete(Ctx("owner", "t-owner"), "ns1"));
    EXPECT_FALSE(svc_->CanDelete(Ctx("u", "t-other"), "ns1"));  // ACL admin cannot delete
    EXPECT_FALSE(svc_->CanTransferOwnership(Ctx("u", "t-other"), "ns1"));
    EXPECT_FALSE(svc_->CanChangeVisibility(Ctx("u", "t-other"), "ns1"));
}

TEST_F(PermissionServiceTest, CanAdminAcl_OwnerOrAdminGrantee) {
    EXPECT_TRUE(svc_->CanAdminAcl(Ctx("owner", "t-owner"), "ns1"));
    Exec("INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at) "
         "VALUES('ns1','t-other',NULL,'admin',0)");
    EXPECT_TRUE(svc_->CanAdminAcl(Ctx("u", "t-other"), "ns1"));
}

// --- UT31: cannot grant to owner tenant ---
TEST_F(PermissionServiceTest, GrantAcl_ToOwnerTenant) {
    auto st = svc_->GrantAcl("ns1", "t-owner", "", NsAclRole::VIEWER, Ctx("owner", "t-owner"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCx(st, "CX_ERR_ACL_GRANT_TO_OWNER_TENANT"));
}

// --- UT32: duplicate grant ---
TEST_F(PermissionServiceTest, GrantAcl_Duplicate) {
    ASSERT_TRUE(svc_->GrantAcl("ns1", "t-other", "", NsAclRole::VIEWER, Ctx("owner", "t-owner")).ok());
    auto st = svc_->GrantAcl("ns1", "t-other", "", NsAclRole::EDITOR, Ctx("owner", "t-owner"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCx(st, "CX_ERR_ACL_DUPLICATE_GRANT"));
}

TEST_F(PermissionServiceTest, GrantAcl_NsNotFound) {
    auto st = svc_->GrantAcl("ns-missing", "t-other", "", NsAclRole::VIEWER, Ctx("owner", "t-owner"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCx(st, "CX_ERR_NS_NOT_FOUND"));
}

TEST_F(PermissionServiceTest, GrantAcl_CallerUnauthorized) {
    // A non-owner, non-admin caller cannot grant.
    auto st = svc_->GrantAcl("ns1", "t-other", "", NsAclRole::VIEWER, Ctx("rando", "t-other"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCx(st, "CX_ERR_NS_UNAUTHORIZED"));
}

// --- UT34: revoke not found ---
TEST_F(PermissionServiceTest, RevokeAcl_NotFound) {
    auto st = svc_->RevokeAcl("ns1", "t-other", "", Ctx("owner", "t-owner"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCx(st, "CX_ERR_ACL_NOT_FOUND"));
}

TEST_F(PermissionServiceTest, GrantThenRevokeThenList) {
    ASSERT_TRUE(svc_->GrantAcl("ns1", "t-other", "", NsAclRole::EDITOR, Ctx("owner", "t-owner")).ok());
    auto list = svc_->ListAcl("ns1", Ctx("owner", "t-owner"));
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list->size(), 1u);
    EXPECT_EQ((*list)[0].grantee_tenant_id, "t-other");
    EXPECT_EQ((*list)[0].role, NsAclRole::EDITOR);
    EXPECT_TRUE((*list)[0].grantee_user_id.empty());  // tenant-level

    ASSERT_TRUE(svc_->RevokeAcl("ns1", "t-other", "", Ctx("owner", "t-owner")).ok());
    auto list2 = svc_->ListAcl("ns1", Ctx("owner", "t-owner"));
    ASSERT_TRUE(list2.ok());
    EXPECT_EQ(list2->size(), 0u);
}

// --- UT35: BatchCheck mixed allow/deny (F04 call pattern) ---
TEST_F(PermissionServiceTest, BatchCheck_Multiple) {
    Exec("INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at) "
         "VALUES('ns1','t-other',NULL,'viewer',0)");
    AuthContext ctx = Ctx("u", "t-other");
    auto res = svc_->BatchCheck(ctx, {"ns1", "ns-pub", "ns-missing"});
    ASSERT_EQ(res.size(), 3u);
    // ns1: viewer grant -> read yes, write no.
    EXPECT_TRUE(res[0].can_read);
    EXPECT_FALSE(res[0].can_write);
    // ns-pub: public -> read yes, write no.
    EXPECT_TRUE(res[1].can_read);
    EXPECT_FALSE(res[1].can_write);
    // ns-missing: nothing -> both no.
    EXPECT_FALSE(res[2].can_read);
    EXPECT_FALSE(res[2].can_write);
}

}  // namespace
}  // namespace cortrix::tenant
