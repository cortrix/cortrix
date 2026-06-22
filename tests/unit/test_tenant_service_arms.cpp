// Coverage-closure (CLOSURE90) unit tests for src/tenant/tenant_service.cpp and
// src/tenant/permission_service.cpp -- the SQLITE_CONSTRAINT / transaction-failed
// "step" arms that test_tenant_service.cpp / test_tenant_coverage_gaps.cpp leave
// uncovered (those files cover the no-db + authz + happy/pre-check arms, but not
// the post-authz INSERT/UPDATE/DELETE failure arms).
//
// Strategy (deterministic, no fault-injection lib): set up a real in-memory
// catalog.db, pass the caller's authz checks normally, then make the *mutating*
// statement fail by either (a) a pre-occupied PK row (CONSTRAINT) or (b) a
// BEFORE trigger that RAISE(ABORT)s -- both yield rc != SQLITE_DONE so the error
// arm runs. The authz/existence queries (which hit other rows/tables) still pass.
//
// Targeted uncovered lines (gcov DA=0):
//   tenant_service.cpp 287-292   -- AddMember INSERT CONSTRAINT + transaction-failed
//   tenant_service.cpp 324-326   -- RemoveMember DELETE step failure
//   tenant_service.cpp 402-403   -- UpdateMemberRole UPDATE step failure
//   tenant_service.cpp 543-550   -- Delete DELETE step failure -> ROLLBACK
//   tenant_service.cpp 593-599   -- TransferOwnership UPDATE step failure -> ROLLBACK
//   permission_service.cpp 220-221 -- RevokeAcl DELETE step failure
//
// Suite names are globally unique + module-prefixed (TenantServiceArms /
// PermissionServiceArms).

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <sqlite3.h>

#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/tenant_error.h"
#include "cortrix/tenant/tenant_service.h"

namespace cortrix::tenant {
namespace {

using catalog::CatalogDb;

bool HasCxArm(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;  // starts_with
}

AuthContext CallerArm(const std::string& user_id, const std::string& tenant_id,
                      bool admin = false) {
    AuthContext c;
    c.user_id = user_id;
    c.tenant_id = tenant_id;
    c.permissions = admin ? kPermAdmin : (kPermRead | kPermWrite);
    return c;
}

// ===========================================================================
// TenantService -- post-authz mutation-failure arms.
// ===========================================================================

class TenantServiceArmsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        svc_ = std::make_unique<TenantService>(catalog_.db());
    }

    void Exec(const std::string& sql) {
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(catalog_.db(), sql.c_str(), nullptr, nullptr, &err), SQLITE_OK)
            << (err ? err : "");
        sqlite3_free(err);
    }

    void SeedUser(const std::string& uid, const std::string& email) {
        Exec("INSERT INTO users(user_id, email, created_at) VALUES('" + uid + "','" + email +
             "',0)");
    }

    std::string MakePersonal(const std::string& uid, const std::string& email) {
        SeedUser(uid, email);
        auto r = svc_->CreatePersonal(uid, email, /*conn=*/nullptr);
        EXPECT_TRUE(r.ok()) << r.status().message();
        return r.ok() ? r->tenant_id : "";
    }

    CatalogDb catalog_;
    std::unique_ptr<TenantService> svc_;
};

// AddMember: the friendly dup pre-check uses ParseTenantRole, which returns
// nullopt for an unrecognized role token -> the pre-check "already a member" path
// is skipped, but the INSERT then hits the (user_id,tenant_id) PK -> the
// SQLITE_CONSTRAINT arm maps it to MEMBER_ALREADY_EXISTS (tenant_service.cpp:288-289).
TEST_F(TenantServiceArmsTest, AddMember_InsertConstraint_MemberAlreadyExists) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("bob", "b@x.com");
    // Pre-occupy (bob,tid) with an UNPARSEABLE role so CallerRole(...).has_value()
    // is false (pre-check passes) yet the PK is already taken.
    Exec("INSERT INTO user_tenants(user_id, tenant_id, role, created_at) VALUES('bob','" + tid +
         "','not_a_real_role',0)");

    auto st = svc_->AddMember(tid, "bob", TenantRole::MEMBER, CallerArm("owner", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxArm(st, "CX_ERR_MEMBER_ALREADY_EXISTS")) << st.message();
}

// RemoveMember: a BEFORE DELETE trigger aborts the DELETE -> rc != SQLITE_DONE ->
// transaction-failed arm (tenant_service.cpp:324-326).
TEST_F(TenantServiceArmsTest, RemoveMember_DeleteFails_TransactionFailed) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("bob", "b@x.com");
    ASSERT_TRUE(svc_->AddMember(tid, "bob", TenantRole::MEMBER, CallerArm("owner", tid)).ok());
    // Authz (owner) + target (bob) lookups succeed; the DELETE itself is forced to fail.
    Exec("CREATE TRIGGER trg_block_del BEFORE DELETE ON user_tenants "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END");

    auto st = svc_->RemoveMember(tid, "bob", CallerArm("owner", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxArm(st, "CX_ERR_TENANT_TRANSACTION_FAILED")) << st.message();
}

// UpdateMemberRole: a BEFORE UPDATE trigger aborts the UPDATE -> rc != SQLITE_DONE
// -> transaction-failed arm (tenant_service.cpp:402-403).
TEST_F(TenantServiceArmsTest, UpdateMemberRole_UpdateFails_TransactionFailed) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("bob", "b@x.com");
    ASSERT_TRUE(svc_->AddMember(tid, "bob", TenantRole::MEMBER, CallerArm("owner", tid)).ok());
    Exec("CREATE TRIGGER trg_block_upd BEFORE UPDATE ON user_tenants "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END");

    auto st = svc_->UpdateMemberRole(tid, "bob", TenantRole::ADMIN, CallerArm("owner", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxArm(st, "CX_ERR_TENANT_TRANSACTION_FAILED")) << st.message();
}

// UpdateName: a BEFORE UPDATE trigger on tenants aborts the UPDATE -> step-fail arm
// (tenant_service.cpp:474-476).
TEST_F(TenantServiceArmsTest, UpdateName_UpdateFails_TransactionFailed) {
    std::string tid = MakePersonal("owner", "o@x.com");
    Exec("CREATE TRIGGER trg_block_tname BEFORE UPDATE ON tenants "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END");

    auto st = svc_->UpdateName(tid, "Renamed", CallerArm("owner", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxArm(st, "CX_ERR_TENANT_TRANSACTION_FAILED")) << st.message();
}

// Delete: tenant is empty (no namespaces / no non-owner members), authz passes,
// BEGIN succeeds, but a BEFORE DELETE trigger on user_tenants aborts the first
// DELETE -> rc1 != SQLITE_DONE -> ROLLBACK + transaction-failed (tenant_service.cpp:543-546).
TEST_F(TenantServiceArmsTest, Delete_DeleteFails_RollbackTransactionFailed) {
    std::string tid = MakePersonal("owner", "o@x.com");
    Exec("CREATE TRIGGER trg_block_del2 BEFORE DELETE ON user_tenants "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END");

    auto st = svc_->Delete(tid, CallerArm("owner", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxArm(st, "CX_ERR_TENANT_TRANSACTION_FAILED")) << st.message();
}

// TransferOwnership: current owner + target member both resolve, BEGIN succeeds,
// then a BEFORE UPDATE trigger on user_tenants aborts the promote/demote UPDATE
// -> rc != SQLITE_DONE -> ROLLBACK + transaction-failed (tenant_service.cpp:593-595).
TEST_F(TenantServiceArmsTest, TransferOwnership_UpdateFails_RollbackTransactionFailed) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("alice", "a@x.com");
    ASSERT_TRUE(svc_->AddMember(tid, "alice", TenantRole::MEMBER, CallerArm("owner", tid)).ok());
    Exec("CREATE TRIGGER trg_block_upd2 BEFORE UPDATE ON user_tenants "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END");

    auto st = svc_->TransferOwnership(tid, "alice", CallerArm("owner", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxArm(st, "CX_ERR_TENANT_TRANSACTION_FAILED")) << st.message();
}

// ===========================================================================
// PermissionService -- RevokeAcl DELETE step-failure arm.
// ===========================================================================

class PermissionServiceArmsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) "
             "VALUES('t-owner','personal','o',0)");
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) "
             "VALUES('t-other','personal','x',0)");
        Exec("INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
             "VALUES('owned-1','owned-1','t-owner','tenant',0)");
        svc_ = std::make_unique<PermissionService>(catalog_.db());
    }

    void Exec(const std::string& sql) {
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(catalog_.db(), sql.c_str(), nullptr, nullptr, &err), SQLITE_OK)
            << (err ? err : "");
        sqlite3_free(err);
    }

    AuthContext Owner() { return CallerArm("owner", "t-owner"); }

    CatalogDb catalog_;
    std::unique_ptr<PermissionService> svc_;
};

// RevokeAcl: owner administers ACL (authz passes), a real grant exists, but a
// BEFORE DELETE trigger on ns_acl aborts the DELETE -> rc != SQLITE_DONE ->
// transaction-failed arm (permission_service.cpp:220-221).
TEST_F(PermissionServiceArmsTest, RevokeAcl_DeleteFails_TransactionFailed) {
    ASSERT_TRUE(svc_->GrantAcl("owned-1", "t-other", "", NsAclRole::VIEWER, Owner()).ok());
    Exec("CREATE TRIGGER trg_block_acl_del BEFORE DELETE ON ns_acl "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END");

    auto st = svc_->RevokeAcl("owned-1", "t-other", "", Owner());
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxArm(st, "CX_ERR_TENANT_TRANSACTION_FAILED")) << st.message();
}

}  // namespace
}  // namespace cortrix::tenant
