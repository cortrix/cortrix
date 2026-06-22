// Coverage-gap unit tests for src/tenant/ (tenant_service / permission_service /
// quota_service). Targets the AUTHORIZATION GUARDS + no-db guards + success/ACL
// paths not already exercised by test_tenant_service.cpp /
// test_tenant_permission_seam.cpp. Runs against a real in-memory catalog.db (F12
// schema) per the project router-test pattern. Deterministic; no network/LLM.
//
// SQLITE_CONSTRAINT / transaction-failed arms require fault injection and are NOT
// covered here (noted in the agent report).
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

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

bool HasCxGap(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;  // starts_with
}

AuthContext CallerGap(const std::string& user_id, const std::string& tenant_id,
                      bool admin = false) {
    AuthContext c;
    c.user_id = user_id;
    c.tenant_id = tenant_id;
    c.permissions = admin ? kPermAdmin : (kPermRead | kPermWrite);
    return c;
}

// ===========================================================================
// TenantService -- no-db guards + remaining authorization branches.
// ===========================================================================

class TenantGapTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        svc_ = std::make_unique<TenantService>(catalog_.db());
    }

    void SeedUser(const std::string& uid, const std::string& email) {
        sqlite3_stmt* s = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(),
                                     "INSERT INTO users(user_id, email, created_at) VALUES(?,?,0)",
                                     -1, &s, nullptr), SQLITE_OK);
        sqlite3_bind_text(s, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, email.c_str(), -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(s), SQLITE_DONE);
        sqlite3_finalize(s);
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

// --- AddMember on a non-existent tenant -> TENANT_NOT_FOUND (existence before authz). ---
TEST_F(TenantGapTest, AddMember_TenantNotFound) {
    auto st = svc_->AddMember("tenant-ghost", "newbie", TenantRole::MEMBER,
                              CallerGap("owner", "tenant-ghost"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_TENANT_NOT_FOUND")) << st.message();
}

// --- RemoveMember by a non-member caller -> CALLER_ROLE_INSUFFICIENT. ---
TEST_F(TenantGapTest, RemoveMember_CallerInsufficient) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("rando", "r@x.com");
    auto st = svc_->RemoveMember(tid, "owner", CallerGap("rando", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_CALLER_ROLE_INSUFFICIENT")) << st.message();
}

// --- UpdateMemberRole by a non-member caller -> CALLER_ROLE_INSUFFICIENT. ---
TEST_F(TenantGapTest, UpdateMemberRole_CallerInsufficient) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("rando", "r@x.com");
    auto st = svc_->UpdateMemberRole(tid, "owner", TenantRole::ADMIN, CallerGap("rando", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_CALLER_ROLE_INSUFFICIENT")) << st.message();
}

// --- UpdateMemberRole on a target that is not a member -> MEMBER_NOT_FOUND. ---
TEST_F(TenantGapTest, UpdateMemberRole_TargetNotFound) {
    std::string tid = MakePersonal("owner", "o@x.com");
    auto st = svc_->UpdateMemberRole(tid, "ghost", TenantRole::ADMIN, CallerGap("owner", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_MEMBER_NOT_FOUND")) << st.message();
}

// --- UpdateMemberRole happy path: promote a member to ADMIN (success arm). ---
TEST_F(TenantGapTest, UpdateMemberRole_Succeeds) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("bob", "b@x.com");
    ASSERT_TRUE(svc_->AddMember(tid, "bob", TenantRole::MEMBER, CallerGap("owner", tid)).ok());
    ASSERT_TRUE(
        svc_->UpdateMemberRole(tid, "bob", TenantRole::ADMIN, CallerGap("owner", tid)).ok());
    auto r = svc_->ListMembers(tid, CallerGap("owner", tid));
    ASSERT_TRUE(r.ok());
    for (const auto& m : *r) {
        if (m.user_id == "bob") EXPECT_EQ(m.role, TenantRole::ADMIN);
    }
}

// --- UpdateName on a non-existent tenant -> TENANT_NOT_FOUND. ---
TEST_F(TenantGapTest, UpdateName_TenantNotFound) {
    auto st = svc_->UpdateName("tenant-ghost", "X", CallerGap("owner", "tenant-ghost"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_TENANT_NOT_FOUND")) << st.message();
}

// --- UpdateName by a non-member caller -> CALLER_ROLE_INSUFFICIENT. ---
TEST_F(TenantGapTest, UpdateName_CallerInsufficient) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("rando", "r@x.com");
    auto st = svc_->UpdateName(tid, "X", CallerGap("rando", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_CALLER_ROLE_INSUFFICIENT")) << st.message();
}

// --- Delete on a non-existent tenant -> TENANT_NOT_FOUND. ---
TEST_F(TenantGapTest, Delete_TenantNotFound) {
    auto st = svc_->Delete("tenant-ghost", CallerGap("owner", "tenant-ghost"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_TENANT_NOT_FOUND")) << st.message();
}

// --- TransferOwnership on a non-existent tenant -> TENANT_NOT_FOUND. ---
TEST_F(TenantGapTest, TransferOwnership_TenantNotFound) {
    auto st =
        svc_->TransferOwnership("tenant-ghost", "alice", CallerGap("owner", "tenant-ghost"));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_TENANT_NOT_FOUND")) << st.message();
}

// --- TransferOwnership by a non-owner -> CALLER_ROLE_INSUFFICIENT. ---
TEST_F(TenantGapTest, TransferOwnership_NotOwner) {
    std::string tid = MakePersonal("owner", "o@x.com");
    SeedUser("bob", "b@x.com");
    ASSERT_TRUE(svc_->AddMember(tid, "bob", TenantRole::ADMIN, CallerGap("owner", tid)).ok());
    // bob is ADMIN, not OWNER -> may not transfer ownership.
    auto st = svc_->TransferOwnership(tid, "bob", CallerGap("bob", tid));
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_CALLER_ROLE_INSUFFICIENT")) << st.message();
}

// --- A null-db TenantService surfaces the transaction-failed "no db" guard
//     across the member/metadata methods (the early db_==nullptr branch). ---
TEST(TenantGapNullDbTest, AllMethodsGuardNoDb) {
    TenantService svc(nullptr);
    AuthContext c = CallerGap("u", "t");
    EXPECT_TRUE(HasCxGap(svc.AddMember("t", "u2", TenantRole::MEMBER, c),
                         "CX_ERR_TENANT_TRANSACTION_FAILED"));
    EXPECT_TRUE(
        HasCxGap(svc.RemoveMember("t", "u2", c), "CX_ERR_TENANT_TRANSACTION_FAILED"));
    EXPECT_TRUE(HasCxGap(svc.UpdateMemberRole("t", "u2", TenantRole::ADMIN, c),
                         "CX_ERR_TENANT_TRANSACTION_FAILED"));
    EXPECT_TRUE(HasCxGap(svc.UpdateName("t", "x", c), "CX_ERR_TENANT_TRANSACTION_FAILED"));
    EXPECT_TRUE(HasCxGap(svc.Delete("t", c), "CX_ERR_TENANT_TRANSACTION_FAILED"));
    EXPECT_TRUE(
        HasCxGap(svc.TransferOwnership("t", "u2", c), "CX_ERR_TENANT_TRANSACTION_FAILED"));
    // Result-returning methods carry the same identity in their status.
    EXPECT_TRUE(HasCxGap(svc.ListMembers("t", c).status(),
                         "CX_ERR_TENANT_TRANSACTION_FAILED"));
    EXPECT_TRUE(HasCxGap(svc.Get("t", c).status(), "CX_ERR_TENANT_TRANSACTION_FAILED"));
    auto org = svc.CreateOrganization("Acme", c);
    EXPECT_TRUE(HasCxGap(org.status(), "CX_ERR_TENANT_TRANSACTION_FAILED"));
}

// ===========================================================================
// PermissionService -- ACL admin success + duplicate/owner/not-found arms.
// ===========================================================================

class PermissionGapTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) VALUES('t-owner','personal','o',0)");
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) VALUES('t-other','personal','x',0)");
        Exec("INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
             "VALUES('owned-1','owned-1','t-owner','tenant',0)");
        Exec("INSERT INTO namespaces(ns_id, name, tenant_id, visibility, created_at) "
             "VALUES('pub-1','pub-1','t-owner','public',0)");
        svc_ = std::make_unique<PermissionService>(catalog_.db());
    }

    void Exec(const char* sql) {
        ASSERT_EQ(sqlite3_exec(catalog_.db(), sql, nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(catalog_.db());
    }

    AuthContext Owner() { return CallerGap("owner", "t-owner"); }

    CatalogDb catalog_;
    std::unique_ptr<PermissionService> svc_;
};

// --- GrantAcl by the owner to a foreign tenant succeeds (the INSERT success arm). ---
TEST_F(PermissionGapTest, GrantAcl_Succeeds) {
    Status st = svc_->GrantAcl("owned-1", "t-other", "", NsAclRole::VIEWER, Owner());
    ASSERT_TRUE(st.ok()) << st.message();
    // The grant is now visible to ListAcl.
    auto r = svc_->ListAcl("owned-1", Owner());
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].grantee_tenant_id, "t-other");
    EXPECT_EQ((*r)[0].role, NsAclRole::VIEWER);
}

// --- GrantAcl to the OWNER tenant is rejected (it already holds full rights). ---
TEST_F(PermissionGapTest, GrantAcl_ToOwnerTenant) {
    Status st = svc_->GrantAcl("owned-1", "t-owner", "", NsAclRole::EDITOR, Owner());
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCxGap(st, "CX_ERR_ACL_GRANT_TO_OWNER_TENANT")) << st.message();
}

// --- A second identical GrantAcl hits the friendly duplicate pre-check. ---
TEST_F(PermissionGapTest, GrantAcl_DuplicateGrant) {
    ASSERT_TRUE(svc_->GrantAcl("owned-1", "t-other", "", NsAclRole::VIEWER, Owner()).ok());
    Status dup = svc_->GrantAcl("owned-1", "t-other", "", NsAclRole::EDITOR, Owner());
    ASSERT_FALSE(dup.ok());
    EXPECT_TRUE(HasCxGap(dup, "CX_ERR_ACL_DUPLICATE_GRANT")) << dup.message();
}

// --- RevokeAcl removes a real grant (success arm) then a re-revoke is ACL_NOT_FOUND. ---
TEST_F(PermissionGapTest, RevokeAcl_SuccessThenNotFound) {
    ASSERT_TRUE(svc_->GrantAcl("owned-1", "t-other", "", NsAclRole::VIEWER, Owner()).ok());
    ASSERT_TRUE(svc_->RevokeAcl("owned-1", "t-other", "", Owner()).ok());
    Status again = svc_->RevokeAcl("owned-1", "t-other", "", Owner());
    ASSERT_FALSE(again.ok());
    EXPECT_TRUE(HasCxGap(again, "CX_ERR_ACL_NOT_FOUND")) << again.message();
}

// --- ListAcl by the owner returns the (empty) list with Ok (success arm). ---
TEST_F(PermissionGapTest, ListAcl_OwnerEmptyOk) {
    auto r = svc_->ListAcl("owned-1", Owner());
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_TRUE(r->empty());
}

// --- A foreign tenant granted EDITOR can read AND write via the ACL composition. ---
TEST_F(PermissionGapTest, EditorGrantEnablesReadAndWrite) {
    ASSERT_TRUE(svc_->GrantAcl("owned-1", "t-other", "", NsAclRole::EDITOR, Owner()).ok());
    AuthContext other = CallerGap("rando", "t-other");
    EXPECT_TRUE(svc_->CanRead(other, "owned-1"));
    EXPECT_TRUE(svc_->CanWrite(other, "owned-1"));   // EDITOR may write
    EXPECT_FALSE(svc_->CanAdminAcl(other, "owned-1"));  // but not administer ACL
}

// --- A VIEWER grant reads but cannot write; a public ns is readable by anyone. ---
TEST_F(PermissionGapTest, ViewerReadOnly_AndPublicReadable) {
    ASSERT_TRUE(svc_->GrantAcl("owned-1", "t-other", "", NsAclRole::VIEWER, Owner()).ok());
    AuthContext other = CallerGap("rando", "t-other");
    EXPECT_TRUE(svc_->CanRead(other, "owned-1"));
    EXPECT_FALSE(svc_->CanWrite(other, "owned-1"));  // VIEWER cannot write
    // A public namespace is readable by a tenant with no grant at all.
    AuthContext stranger = CallerGap("nobody", "t-stranger");
    EXPECT_TRUE(svc_->CanRead(stranger, "pub-1"));
    EXPECT_FALSE(svc_->CanWrite(stranger, "pub-1"));  // public is read-only
}

// --- A null-db PermissionService denies every boolean path and "no db"-guards
//     the mutating ACL ops. ---
TEST(PermissionGapNullDbTest, NullDbDeniesAndGuards) {
    PermissionService svc(nullptr);
    AuthContext c = CallerGap("u", "t");
    EXPECT_FALSE(svc.CanRead(c, "ns"));
    EXPECT_FALSE(svc.CanWrite(c, "ns"));
    EXPECT_FALSE(svc.CanAdminAcl(c, "ns"));
    EXPECT_TRUE(HasCxGap(svc.GrantAcl("ns", "t2", "", NsAclRole::VIEWER, c),
                         "CX_ERR_TENANT_TRANSACTION_FAILED"));
    EXPECT_TRUE(HasCxGap(svc.RevokeAcl("ns", "t2", "", c),
                         "CX_ERR_TENANT_TRANSACTION_FAILED"));
    EXPECT_TRUE(
        HasCxGap(svc.ListAcl("ns", c).status(), "CX_ERR_TENANT_TRANSACTION_FAILED"));
}

// ===========================================================================
// QuotaService -- provider + tenant-existence branches.
// ===========================================================================

// A provider with a fixed finite ceiling (so the over-quota branch is reachable;
// the default UnlimitedPlanProvider always returns true).
class FixedPlanProvider : public IPlanProvider {
public:
    explicit FixedPlanProvider(int64_t limit) : limit_(limit) {}
    QuotaLimit GetLimit(const TenantId&, QuotaType) override { return {limit_}; }

private:
    int64_t limit_;
};

class QuotaGapTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        Exec("INSERT INTO tenants(tenant_id, type, name, created_at) VALUES('t-real','personal','r',0)");
    }
    void Exec(const char* sql) {
        ASSERT_EQ(sqlite3_exec(catalog_.db(), sql, nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(catalog_.db());
    }
    CatalogDb catalog_;
};

// --- GetQuota on a missing tenant (db handle present) -> TENANT_NOT_FOUND. ---
TEST_F(QuotaGapTest, GetQuota_TenantNotFound) {
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), catalog_.db());
    auto r = svc.GetQuota("tenant-missing", CallerGap("u", "t"));
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCxGap(r.status(), "CX_ERR_TENANT_NOT_FOUND")) << r.status().message();
}

// --- GetQuota on a real tenant reports all five types (unlimited usage=0). ---
TEST_F(QuotaGapTest, GetQuota_Success) {
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), catalog_.db());
    auto r = svc.GetQuota("t-real", CallerGap("u", "t"));
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r->tenant_id, "t-real");
    EXPECT_EQ(r->limits.size(), 5u);
    EXPECT_EQ(r->usage.size(), 5u);
    EXPECT_TRUE(r->limits.at(QuotaType::MAX_NAMESPACES).IsUnlimited());
    EXPECT_EQ(r->usage.at(QuotaType::MAX_NAMESPACES), 0);
}

// --- CheckQuota: unlimited provider always passes; a finite provider enforces. ---
TEST_F(QuotaGapTest, CheckQuota_UnlimitedVsOverLimit) {
    QuotaService unlimited(std::make_shared<UnlimitedPlanProvider>(), catalog_.db());
    EXPECT_TRUE(unlimited.CheckQuota("t-real", QuotaType::MAX_QPS, 1000000));

    QuotaService finite(std::make_shared<FixedPlanProvider>(10), catalog_.db());
    EXPECT_TRUE(finite.CheckQuota("t-real", QuotaType::MAX_QPS, 10));    // at the ceiling
    EXPECT_FALSE(finite.CheckQuota("t-real", QuotaType::MAX_QPS, 11));   // over -> denied
}

// --- UpdateQuotaOverride validates existence: missing -> NOT_FOUND, real -> Ok. ---
TEST_F(QuotaGapTest, UpdateQuotaOverride_ExistenceGate) {
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), catalog_.db());
    auto miss = svc.UpdateQuotaOverride("tenant-missing", QuotaType::MAX_QPS, {5},
                                        CallerGap("a", "t", /*admin=*/true));
    ASSERT_FALSE(miss.ok());
    EXPECT_TRUE(HasCxGap(miss, "CX_ERR_TENANT_NOT_FOUND")) << miss.message();

    auto ok = svc.UpdateQuotaOverride("t-real", QuotaType::MAX_QPS, {5},
                                      CallerGap("a", "t", /*admin=*/true));
    EXPECT_TRUE(ok.ok()) << ok.message();
}

// --- RecordUsage is a V1.0 OSS no-op (never throws / never writes). ---
TEST_F(QuotaGapTest, RecordUsage_IsNoOp) {
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), catalog_.db());
    svc.RecordUsage("t-real", QuotaType::MAX_STORAGE_BYTES, 123);
    // Still reports usage=0 afterward (no persistence in V1.0 OSS).
    auto r = svc.GetQuota("t-real", CallerGap("u", "t"));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->usage.at(QuotaType::MAX_STORAGE_BYTES), 0);
}

// --- With a null db handle, GetQuota skips the existence check (provider-only). ---
TEST(QuotaGapNullDbTest, NullDbSkipsExistenceCheck) {
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), /*catalog_db=*/nullptr);
    auto r = svc.GetQuota("anything", CallerGap("u", "t"));
    EXPECT_TRUE(r.ok()) << r.status().message();
}

}  // namespace
}  // namespace cortrix::tenant
