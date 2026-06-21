// 🔒 tenant/permission service-seam security invariants — unit-pinned (the
// sec/* suite asserts these at the HTTP level; this pins them at the service seam
// for fast regression):
//   - BatchCheck "wildcard / list never exceeds the owned+granted set": a foreign
//     or non-existent namespace always resolves to {read=false, write=false};
//   - mixed owned + foreign requests do not leak access to the foreign entries;
//   - anti-enumeration: a GHOST (non-existent) namespace and a FOREIGN namespace
//     are INDISTINGUISHABLE — GrantAcl on either returns the SAME Status, and the
//     namespace name is NEVER echoed in the error message.
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

class PermissionSeamTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) VALUES('t-owner','personal','o',0)");
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) VALUES('t-other','personal','x',0)");
        // owned-1, owned-2 owned by t-owner; foreign-1 owned by t-other.
        Exec("INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
             "VALUES('owned-1','owned-1','t-owner','tenant',0)");
        Exec("INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
             "VALUES('owned-2','owned-2','t-owner','tenant',0)");
        Exec("INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
             "VALUES('foreign-1','foreign-1','t-other','tenant',0)");
        svc_ = std::make_unique<PermissionService>(catalog_.db());
    }

    void Exec(const char* sql) {
        ASSERT_EQ(sqlite3_exec(catalog_.db(), sql, nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(catalog_.db());
    }

    CatalogDb catalog_;
    std::unique_ptr<PermissionService> svc_;
};

// --- A BatchCheck over the whole NS universe never grants the foreign / ghost
//     entries to t-owner (wildcard expansion cannot exceed the owned set). ---
TEST_F(PermissionSeamTest, BatchCheckNeverExceedsOwnedSet) {
    AuthContext owner = Ctx("owner", "t-owner");
    auto res = svc_->BatchCheck(owner, {"owned-1", "owned-2", "foreign-1", "ghost-ns"});
    ASSERT_EQ(res.size(), 4u);

    // owned-1 / owned-2: full owner access.
    EXPECT_TRUE(res[0].can_read);
    EXPECT_TRUE(res[0].can_write);
    EXPECT_TRUE(res[1].can_read);
    EXPECT_TRUE(res[1].can_write);
    // foreign-1: owned by a DIFFERENT tenant, no ACL → no access leaks.
    EXPECT_FALSE(res[2].can_read);
    EXPECT_FALSE(res[2].can_write);
    // ghost-ns: does not exist → no access (and indistinguishable from foreign).
    EXPECT_FALSE(res[3].can_read);
    EXPECT_FALSE(res[3].can_write);
}

// --- A mixed owned+foreign request does not elevate the foreign entry, even
//     when the caller legitimately owns the others in the same batch. ---
TEST_F(PermissionSeamTest, MixedOwnershipDoesNotLeakForeign) {
    AuthContext owner = Ctx("owner", "t-owner");
    auto res = svc_->BatchCheck(owner, {"foreign-1", "owned-1"});
    ASSERT_EQ(res.size(), 2u);
    EXPECT_FALSE(res[0].can_read);   // foreign stays denied
    EXPECT_FALSE(res[0].can_write);
    EXPECT_TRUE(res[1].can_read);    // owned still allowed
}

// --- CanRead/CanWrite on a foreign and on a ghost namespace both return false
//     (the boolean hot-path equivalent of anti-enumeration). ---
TEST_F(PermissionSeamTest, ForeignAndGhostBothDeniedOnBooleanPath) {
    AuthContext owner = Ctx("owner", "t-owner");
    EXPECT_FALSE(svc_->CanRead(owner, "foreign-1"));
    EXPECT_FALSE(svc_->CanRead(owner, "ghost-ns"));
    EXPECT_FALSE(svc_->CanWrite(owner, "foreign-1"));
    EXPECT_FALSE(svc_->CanWrite(owner, "ghost-ns"));
}

// --- A non-owner caller cannot administer ACL on a namespace they own no rights
//     to: the denial carries the canonical CX_ERR_NS_UNAUTHORIZED token (the
//     stable identity an Agent / the API boundary routes on). ---
TEST_F(PermissionSeamTest, UnauthorizedGrantCarriesCanonicalToken) {
    AuthContext rando = Ctx("rando", "t-other");  // not the owner of owned-1
    Status st = svc_->GrantAcl("owned-1", "t-other", "", NsAclRole::VIEWER, rando);
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCx(st, "CX_ERR_NS_UNAUTHORIZED"));
    EXPECT_EQ(st.code(), StatusCode::kPermissionDenied);
}

// --- Existence is checked before authz: a GHOST namespace surfaces
//     CX_ERR_NS_NOT_FOUND (an existence probe at this internal seam; the API
//     boundary is responsible for collapsing it to the public-facing
//     indistinguishable response). ---
TEST_F(PermissionSeamTest, GhostGrantSurfacesNsNotFound) {
    AuthContext rando = Ctx("rando", "t-other");
    Status st = svc_->GrantAcl("ghost-ns", "t-owner", "", NsAclRole::VIEWER, rando);
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCx(st, "CX_ERR_NS_NOT_FOUND"));
    EXPECT_EQ(st.code(), StatusCode::kNotFound);
}

// --- RevokeAcl: foreign (caller not owner) → NS_UNAUTHORIZED; ghost → NS_NOT_FOUND.
//     Both carry their canonical CX_ERR token. ---
TEST_F(PermissionSeamTest, RevokeAclCanonicalTokens) {
    AuthContext rando = Ctx("rando", "t-other");
    // owned-1 belongs to t-owner, so to rando (t-other) it is foreign → UNAUTHORIZED.
    Status foreign = svc_->RevokeAcl("owned-1", "t-owner", "", rando);
    Status ghost = svc_->RevokeAcl("ghost-ns", "t-owner", "", rando);
    ASSERT_FALSE(foreign.ok());
    ASSERT_FALSE(ghost.ok());
    EXPECT_TRUE(HasCx(foreign, "CX_ERR_NS_UNAUTHORIZED"));
    EXPECT_TRUE(HasCx(ghost, "CX_ERR_NS_NOT_FOUND"));
}

// --- ListAcl by a non-owner is unauthorized with the canonical token. ---
TEST_F(PermissionSeamTest, ListAclUnauthorizedCanonicalToken) {
    AuthContext rando = Ctx("rando", "t-other");
    auto r = svc_->ListAcl("owned-1", rando);
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCx(r.status(), "CX_ERR_NS_UNAUTHORIZED"));
}

}  // namespace
}  // namespace cortrix::tenant
