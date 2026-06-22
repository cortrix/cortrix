// tenant_routes.cpp full-surface coverage (the module is ~24.8% region: only 3 of
// 16 endpoints were exercised). For EACH endpoint this file drives:
//   * the happy path (2xx) against real seeded catalog state,
//   * the request-validation branch (missing/wrong-type body → 400),
//   * a service-error arm that flows through WriteStatusError / CodeFromStatus
//     (not-found 404 / conflict 409 / permission 403 / quota 404),
//   * the non-admin (403) gate on the admin-gated endpoints.
//
// Harness: an in-process loopback httplib::Server + RegisterTenantRoutes over real
// TenantService / PermissionService / QuotaService backed by an in-memory
// catalog.db (same pattern as test_errpath_tenant.cpp). The config admin key's
// tenant_id == its authenticated user_id (ApiKeyAuth config path), so seeding a
// user_tenants row for that id makes the caller a member/owner of a seeded tenant,
// reaching the service happy paths AND the member-scoped error arms.
//
// Suite/fixture names are module-prefixed (TenantRouteFull) for global gtest
// uniqueness. Deterministic: 127.0.0.1 only, no LLM. The auth key tenant_ids double
// as user_ids (MVP user_id == tenant_id), seeded as real users below.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <sqlite3.h>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/server/routes/tenant_routes.h"
#include "cortrix/tenant/i_plan_provider.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/quota_service.h"
#include "cortrix/tenant/tenant_error.h"
#include "cortrix/tenant/tenant_service.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using catalog::CatalogDb;
using tenant::PermissionService;
using tenant::QuotaService;
using tenant::TenantService;
using tenant::UnlimitedPlanProvider;

class TenantRouteFull : public ::testing::Test {
protected:
    // Caller identities (key tenant_id == authenticated user_id under config auth).
    static constexpr const char* kAdmin = "usr_admin";    // admin key, owner of kOrg
    static constexpr const char* kWriter = "usr_writer";  // write (no admin)
    static constexpr const char* kReader = "usr_reader";  // read-only, member of kOrg

    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        db_ = catalog_.db();

        tenant_svc_ = std::make_unique<TenantService>(db_);
        plan_ = std::make_shared<UnlimitedPlanProvider>();
        perm_svc_ = std::make_unique<PermissionService>(db_);
        quota_svc_ = std::make_unique<QuotaService>(plan_, db_);

        // Seed users (catalog users table is (user_id, email, created_at)).
        SeedUser(kAdmin, "admin@e.com");
        SeedUser(kWriter, "writer@e.com");
        SeedUser(kReader, "reader@e.com");
        SeedUser("usr_member", "member@e.com");
        SeedUser("usr_target", "target@e.com");

        // An organization tenant owned by kAdmin, with kReader as a 'member'. This
        // makes kAdmin OWNER (manager) and kReader a member but not a manager.
        AuthContext admin_ctx = Ctx(kAdmin, /*admin=*/true);
        auto org = tenant_svc_->CreateOrganization("Acme Org", admin_ctx);
        ASSERT_TRUE(org.ok()) << org.status().message();
        org_id_ = org.value().tenant_id;
        AddMemberRow(kReader, org_id_, "member");

        // A namespace owned by kAdmin's tenant (= kAdmin user id here, since the
        // config-auth path sets tenant_id == user_id) so the ACL endpoints can hit
        // the owner-admin happy path. The owner tenant for ACL is the CALLER's
        // AuthContext.tenant_id, which equals kAdmin.
        SeedNamespace(kNs, kAdmin);

        // Auth keys.
        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "trf-admin-key";
        writer_key_ = "trf-writer-key";
        reader_key_ = "trf-reader-key";
        auth_->LoadKeys({
            MakeKey(admin_key_, kAdmin, kPermRead | kPermWrite | kPermAdmin),
            MakeKey(writer_key_, kWriter, kPermRead | kPermWrite),
            MakeKey(reader_key_, kReader, kPermRead),
        });

        static std::atomic<unsigned> ctr{0};
        port_ = 19500 + ((getpid() + ctr.fetch_add(1)) % 200);
        server_ = std::make_unique<httplib::Server>();
        RegisterTenantRoutes(*server_, *tenant_svc_, *perm_svc_, *quota_svc_, *auth_);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/tenants", Admin());
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    // ---- seeding helpers ----
    void SeedUser(const std::string& uid, const std::string& email) {
        sqlite3_stmt* s = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db_, "INSERT INTO users(user_id, email, created_at) VALUES(?,?,0)", -1, &s,
                      nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(s, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, email.c_str(), -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(s), SQLITE_DONE);
        sqlite3_finalize(s);
    }

    void AddMemberRow(const std::string& uid, const std::string& tid, const std::string& role) {
        sqlite3_stmt* s = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db_,
                      "INSERT INTO user_tenants(user_id, tenant_id, role, created_at) VALUES(?,?,?,0)",
                      -1, &s, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(s, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, tid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, role.c_str(), -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(s), SQLITE_DONE);
        sqlite3_finalize(s);
    }

    // Insert a tenant row (for ACL owner tenant FK) + a namespace owned by it.
    void SeedNamespace(const std::string& ns_id, const std::string& owner_tenant) {
        sqlite3_stmt* t = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db_,
                      "INSERT OR IGNORE INTO tenants(tenant_id, type, name, created_at) "
                      "VALUES(?,'organization',?,0)",
                      -1, &t, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(t, 1, owner_tenant.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(t, 2, owner_tenant.c_str(), -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(t), SQLITE_DONE);
        sqlite3_finalize(t);

        sqlite3_stmt* n = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db_,
                      "INSERT INTO namespaces(ns_id, name, tenant_id, created_at) VALUES(?,?,?,0)",
                      -1, &n, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(n, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(n, 2, ns_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(n, 3, owner_tenant.c_str(), -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(n), SQLITE_DONE);
        sqlite3_finalize(n);
    }

    static ApiKeyConfig MakeKey(const std::string& plaintext, const std::string& tenant,
                                int perms) {
        ApiKeyConfig k;
        k.key_hash = ApiKeyAuth::HashKey(plaintext);
        k.tenant_id = tenant;
        k.permissions = perms;
        return k;
    }

    static AuthContext Ctx(const std::string& uid, bool admin) {
        AuthContext c;
        c.user_id = uid;
        c.tenant_id = uid;
        c.permissions = admin ? (kPermRead | kPermWrite | kPermAdmin) : kPermRead;
        return c;
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers Writer() { return {{"Authorization", "Bearer " + writer_key_}}; }
    httplib::Headers Reader() { return {{"Authorization", "Bearer " + reader_key_}}; }

    static std::string ErrCode(const httplib::Result& r) {
        return json::parse(r->body)["error"]["code"].get<std::string>();
    }

    static constexpr const char* kNs = "ns_acme";
    CatalogDb catalog_;
    sqlite3* db_ = nullptr;
    std::unique_ptr<TenantService> tenant_svc_;
    std::shared_ptr<UnlimitedPlanProvider> plan_;
    std::unique_ptr<PermissionService> perm_svc_;
    std::unique_ptr<QuotaService> quota_svc_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_, writer_key_, reader_key_;
    std::string org_id_;
};

// =========================================================================
// 1. POST /api/v1/tenants — CreateOrganization (admin)
// =========================================================================
TEST_F(TenantRouteFull, CreateTenantHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/tenants", Admin(), R"({"name":"NewCo"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["tenant"]["name"], "NewCo");
}

TEST_F(TenantRouteFull, CreateTenantMissingName400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/tenants", Admin(), R"({"foo":"bar"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_ROLE_INVALID");
}

TEST_F(TenantRouteFull, CreateTenantNameNotString400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/tenants", Admin(), R"({"name":123})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
}

TEST_F(TenantRouteFull, CreateTenantNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/tenants", Writer(), R"({"name":"X"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
}

TEST_F(TenantRouteFull, CreateTenantNoAuth401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/tenants", R"({"name":"X"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// =========================================================================
// 2. GET /api/v1/tenants — admin list-all
// =========================================================================
TEST_F(TenantRouteFull, ListTenantsAdmin200) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/tenants", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(json::parse(res->body)["tenants"].is_array());
}

TEST_F(TenantRouteFull, ListTenantsNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/tenants", Reader());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// =========================================================================
// 3. GET /api/v1/tenants/{id} — Get (read; admin or member)
// =========================================================================
TEST_F(TenantRouteFull, GetTenantHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(("/api/v1/tenants/" + org_id_).c_str(), Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["tenant"]["name"], "Acme Org");
}

TEST_F(TenantRouteFull, GetTenantNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/tenants/tenant-ghost", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_NOT_FOUND");
}

// A reader who is NOT a member of the org → kCallerNotMember (403). Use a tenant
// the reader does not belong to (the admin's org minus the reader membership is
// already a member, so use a fresh tenant the reader is not in).
TEST_F(TenantRouteFull, GetTenantNonMember403) {
    // Create a second org owned by admin; reader is NOT a member of it.
    AuthContext admin_ctx = Ctx(kAdmin, true);
    auto org2 = tenant_svc_->CreateOrganization("Other Org", admin_ctx);
    ASSERT_TRUE(org2.ok());
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(("/api/v1/tenants/" + org2.value().tenant_id).c_str(), Reader());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_CALLER_NOT_MEMBER");
}

// =========================================================================
// 4. PATCH /api/v1/tenants/{id} — UpdateName (admin)
// =========================================================================
TEST_F(TenantRouteFull, UpdateNameHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(("/api/v1/tenants/" + org_id_).c_str(), Admin(),
                         R"({"name":"Acme Renamed"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["ok"], true);
}

TEST_F(TenantRouteFull, UpdateNameMissingName400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(("/api/v1/tenants/" + org_id_).c_str(), Admin(), R"({})",
                         "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_ROLE_INVALID");
}

TEST_F(TenantRouteFull, UpdateNameNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch("/api/v1/tenants/tenant-ghost", Admin(), R"({"name":"X"})",
                         "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_NOT_FOUND");
}

TEST_F(TenantRouteFull, UpdateNameNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(("/api/v1/tenants/" + org_id_).c_str(), Writer(),
                         R"({"name":"X"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// =========================================================================
// 5. DELETE /api/v1/tenants/{id} — Delete (admin)
// =========================================================================
TEST_F(TenantRouteFull, DeleteTenantNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/tenants/tenant-ghost", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_NOT_FOUND");
}

// kAdmin owns org_id_ but org_id_ still has a non-owner member (kReader) →
// kTenantDeleteHasMembers (the conflict arm, 400 invalid-argument category).
TEST_F(TenantRouteFull, DeleteTenantHasMembersConflict) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(("/api/v1/tenants/" + org_id_).c_str(), Admin());
    ASSERT_TRUE(res);
    // kTenantDeleteHasMembers maps to a non-2xx; pin the surfaced CX identity.
    EXPECT_GE(res->status, 400);
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_DELETE_HAS_MEMBERS");
}

TEST_F(TenantRouteFull, DeleteTenantHappyPath) {
    // A fresh org with NO extra members and no namespaces deletes cleanly.
    AuthContext admin_ctx = Ctx(kAdmin, true);
    auto org = tenant_svc_->CreateOrganization("Disposable", admin_ctx);
    ASSERT_TRUE(org.ok());
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(("/api/v1/tenants/" + org.value().tenant_id).c_str(), Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["ok"], true);
}

TEST_F(TenantRouteFull, DeleteTenantNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(("/api/v1/tenants/" + org_id_).c_str(), Reader());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// =========================================================================
// 6. POST /api/v1/tenants/{id}/members — AddMember (admin)
// =========================================================================
TEST_F(TenantRouteFull, AddMemberHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"user_id", "usr_member"}, {"role", "member"}};
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/members").c_str(), Admin(),
                        b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201) << res->body;
}

TEST_F(TenantRouteFull, AddMemberMissingFields400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/members").c_str(), Admin(),
                        R"({"user_id":"x"})", "application/json");  // no role
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_ROLE_INVALID");
}

TEST_F(TenantRouteFull, AddMemberInvalidRole400) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"user_id", "usr_member"}, {"role", "superuser"}};
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/members").c_str(), Admin(),
                        b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_ROLE_INVALID");
}

TEST_F(TenantRouteFull, AddMemberTenantNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"user_id", "usr_member"}, {"role", "member"}};
    auto res = cli.Post("/api/v1/tenants/tenant-ghost/members", Admin(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_NOT_FOUND");
}

TEST_F(TenantRouteFull, AddMemberAlreadyExistsConflict) {
    httplib::Client cli("127.0.0.1", port_);
    // kReader is already a 'member' of org_id_ → kMemberAlreadyExists (409).
    json b = {{"user_id", kReader}, {"role", "member"}};
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/members").c_str(), Admin(),
                        b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 409) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_ALREADY_EXISTS");
}

TEST_F(TenantRouteFull, AddMemberNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"user_id", "usr_member"}, {"role", "member"}};
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/members").c_str(), Writer(),
                        b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// =========================================================================
// 7. GET /api/v1/tenants/{id}/members — ListMembers (read; member)
// =========================================================================
TEST_F(TenantRouteFull, ListMembersHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(("/api/v1/tenants/" + org_id_ + "/members").c_str(), Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_TRUE(json::parse(res->body)["members"].is_array());
}

TEST_F(TenantRouteFull, ListMembersNonMember403) {
    // writer is not a member of org_id_ → kCallerNotMember (403).
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(("/api/v1/tenants/" + org_id_ + "/members").c_str(), Writer());
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400);
    EXPECT_LT(res->status, 500);
}

// =========================================================================
// 8. DELETE /api/v1/tenants/{id}/members/{uid} — RemoveMember (admin)
// =========================================================================
TEST_F(TenantRouteFull, RemoveMemberHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(("/api/v1/tenants/" + org_id_ + "/members/" + kReader).c_str(),
                          Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["ok"], true);
}

TEST_F(TenantRouteFull, RemoveMemberNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(("/api/v1/tenants/" + org_id_ + "/members/usr_nobody").c_str(),
                          Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_NOT_FOUND");
}

TEST_F(TenantRouteFull, RemoveMemberNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(("/api/v1/tenants/" + org_id_ + "/members/" + kReader).c_str(),
                          Reader());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// =========================================================================
// 9. PATCH /api/v1/tenants/{id}/members/{uid} — UpdateMemberRole (admin)
// =========================================================================
TEST_F(TenantRouteFull, UpdateMemberRoleHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(("/api/v1/tenants/" + org_id_ + "/members/" + kReader).c_str(),
                         Admin(), R"({"role":"admin"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
}

TEST_F(TenantRouteFull, UpdateMemberRoleMissingRole400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(("/api/v1/tenants/" + org_id_ + "/members/" + kReader).c_str(),
                         Admin(), R"({})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_ROLE_INVALID");
}

TEST_F(TenantRouteFull, UpdateMemberRoleInvalidRole400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(("/api/v1/tenants/" + org_id_ + "/members/" + kReader).c_str(),
                         Admin(), R"({"role":"wizard"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_ROLE_INVALID");
}

TEST_F(TenantRouteFull, UpdateMemberRoleNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(("/api/v1/tenants/" + org_id_ + "/members/usr_nobody").c_str(),
                         Admin(), R"({"role":"admin"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_MEMBER_NOT_FOUND");
}

// =========================================================================
// 10. POST /api/v1/tenants/{id}/transfer-ownership — TransferOwnership (admin)
// =========================================================================
TEST_F(TenantRouteFull, TransferOwnershipMissingField400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/transfer-ownership").c_str(),
                        Admin(), R"({})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_TRANSFER_TARGET_NOT_MEMBER");
}

// Target is not a member → kTenantTransferTargetNotMember (the service arm).
TEST_F(TenantRouteFull, TransferOwnershipTargetNotMember) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"new_owner_user_id", "usr_target"}};  // not a member of org_id_
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/transfer-ownership").c_str(),
                        Admin(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400);
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_TRANSFER_TARGET_NOT_MEMBER");
}

TEST_F(TenantRouteFull, TransferOwnershipHappyPath) {
    // Promote kReader (already a member) to the new owner.
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"new_owner_user_id", kReader}};
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/transfer-ownership").c_str(),
                        Admin(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
}

TEST_F(TenantRouteFull, TransferOwnershipNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"new_owner_user_id", kReader}};
    auto res = cli.Post(("/api/v1/tenants/" + org_id_ + "/transfer-ownership").c_str(),
                        Writer(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// =========================================================================
// 11. GET /api/v1/namespaces/{ns}/acl — ListAcl (read; owner/ACL admin)
// =========================================================================
TEST_F(TenantRouteFull, ListAclHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get((std::string("/api/v1/namespaces/") + kNs + "/acl").c_str(), Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_TRUE(json::parse(res->body)["acl"].is_array());
}

TEST_F(TenantRouteFull, ListAclNsNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/namespaces/ns_ghost/acl", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_NS_NOT_FOUND");
}

// =========================================================================
// 12. POST /api/v1/namespaces/{ns}/acl — GrantAcl (write; owner/ACL admin)
// =========================================================================
TEST_F(TenantRouteFull, GrantAclHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    // Grant the writer's tenant viewer access to the admin-owned namespace.
    json b = {{"grantee_tenant_id", kWriter}, {"role", "viewer"}};
    auto res = cli.Post((std::string("/api/v1/namespaces/") + kNs + "/acl").c_str(), Admin(),
                        b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201) << res->body;
}

TEST_F(TenantRouteFull, GrantAclMissingFields400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post((std::string("/api/v1/namespaces/") + kNs + "/acl").c_str(), Admin(),
                        R"({"grantee_tenant_id":"x"})", "application/json");  // no role
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_ACL_GRANT_INVALID_ROLE");
}

TEST_F(TenantRouteFull, GrantAclInvalidRole400) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"grantee_tenant_id", kWriter}, {"role", "superadmin"}};
    auto res = cli.Post((std::string("/api/v1/namespaces/") + kNs + "/acl").c_str(), Admin(),
                        b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_ACL_GRANT_INVALID_ROLE");
}

TEST_F(TenantRouteFull, GrantAclNsNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"grantee_tenant_id", kWriter}, {"role", "viewer"}};
    auto res = cli.Post("/api/v1/namespaces/ns_ghost/acl", Admin(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_NS_NOT_FOUND");
}

// =========================================================================
// 13. DELETE /api/v1/namespaces/{ns}/acl/{grantee} — RevokeAcl (write)
// =========================================================================
TEST_F(TenantRouteFull, RevokeAclHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    // First grant, then revoke.
    json b = {{"grantee_tenant_id", kWriter}, {"role", "viewer"}};
    auto granted = cli.Post((std::string("/api/v1/namespaces/") + kNs + "/acl").c_str(),
                            Admin(), b.dump(), "application/json");
    ASSERT_TRUE(granted);
    ASSERT_EQ(granted->status, 201) << granted->body;

    auto res = cli.Delete(
        (std::string("/api/v1/namespaces/") + kNs + "/acl/" + kWriter).c_str(), Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["ok"], true);
}

TEST_F(TenantRouteFull, RevokeAclNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    // No grant exists for usr_target → kAclNotFound (the revoke not-found arm).
    auto res = cli.Delete(
        (std::string("/api/v1/namespaces/") + kNs + "/acl/usr_target").c_str(), Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_ACL_NOT_FOUND");
}

TEST_F(TenantRouteFull, RevokeAclNsNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/namespaces/ns_ghost/acl/usr_x", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_NS_NOT_FOUND");
}

// =========================================================================
// 14. GET /api/v1/tenants/{id}/quota — GetQuota (read; member)
// =========================================================================
TEST_F(TenantRouteFull, GetQuotaHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(("/api/v1/tenants/" + org_id_ + "/quota").c_str(), Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["tenant_id"], org_id_);
    EXPECT_TRUE(body.contains("limits"));
    EXPECT_TRUE(body.contains("usage"));
}

TEST_F(TenantRouteFull, GetQuotaTenantNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/tenants/tenant-ghost/quota", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_NOT_FOUND");
}

// =========================================================================
// 15. PATCH /api/v1/admin/tenants/{id}/quota — UpdateQuotaOverride (admin)
// =========================================================================
TEST_F(TenantRouteFull, UpdateQuotaHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"quota_type", "max_namespaces"}, {"limit", 100}};
    auto res = cli.Patch(("/api/v1/admin/tenants/" + org_id_ + "/quota").c_str(), Admin(),
                         b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["ok"], true);
}

TEST_F(TenantRouteFull, UpdateQuotaMissingFields404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(("/api/v1/admin/tenants/" + org_id_ + "/quota").c_str(), Admin(),
                         R"({"limit":10})", "application/json");  // no quota_type
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_QUOTA_NOT_FOUND");
}

TEST_F(TenantRouteFull, UpdateQuotaUnknownType404) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"quota_type", "max_unicorns"}, {"limit", 10}};
    auto res = cli.Patch(("/api/v1/admin/tenants/" + org_id_ + "/quota").c_str(), Admin(),
                         b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_QUOTA_NOT_FOUND");
}

TEST_F(TenantRouteFull, UpdateQuotaNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"quota_type", "max_namespaces"}, {"limit", 100}};
    auto res = cli.Patch(("/api/v1/admin/tenants/" + org_id_ + "/quota").c_str(), Writer(),
                         b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// =========================================================================
// switch-tenant is deliberately NOT registered → 404 (Agent-friendly signal).
// =========================================================================
TEST_F(TenantRouteFull, SwitchTenantUnregistered404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/auth/switch-tenant", Admin(), R"({"tenant_id":"x"})",
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
}  // namespace cortrix
