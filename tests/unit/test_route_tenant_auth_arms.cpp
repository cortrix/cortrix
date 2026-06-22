// Remaining error/validation-arm coverage for tenant_routes.cpp + auth_routes.cpp,
// complementing the existing full-surface suites (test_route_tenant_full.cpp /
// test_route_auth_full.cpp / test_admin_users_routes.cpp). Only the arms those
// suites leave uncovered (SERVER_WAVE_GAPS.md) are driven here. Deterministic
// in-process loopback httplib servers, 127.0.0.1 only, no LLM.
//
//   * TenantRoutesArms — the ACL serializer (AclToJson) reached by listing a
//     populated ACL, the GrantAcl grantee_user_id body branch, and the
//     UpdateQuotaOverride service-error arm (WriteStatusError on a ghost tenant).
//   * AuthRoutesArms — ApiKeyInfoToJson's non-null expires_at/last_used_at branch
//     (create-with-expiry then list), the bootstrap POST wrong-token Consume
//     failure, the admin-user PATCH malformed-JSON 422 arm, and LogAdminUserAction
//     with a real (non-null) operation logger.
//
// Suite prefixes (TenantRoutesArms / AuthRoutesArms) are globally unique.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/admin_users_service.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/auth/bootstrap_handler.h"
#include "cortrix/auth/pbkdf2_password_hasher.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/server/routes/auth_routes.h"
#include "cortrix/server/routes/tenant_routes.h"
#include "cortrix/tenant/i_plan_provider.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/quota_service.h"
#include "cortrix/tenant/tenant_service.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

int ArmsPort2(int base) {
    static std::atomic<unsigned> ctr{0};
    return base + ((getpid() + ctr.fetch_add(1)) % 200);
}

static ApiKeyConfig MakeKey2(const std::string& plaintext, const std::string& tenant, int perms) {
    ApiKeyConfig k;
    k.key_hash = ApiKeyAuth::HashKey(plaintext);
    k.tenant_id = tenant;
    k.permissions = perms;
    return k;
}

// ===========================================================================
// TenantRoutesArms — ACL serializer + GrantAcl user-scoped branch + quota error.
// ===========================================================================
class TenantRoutesArms : public ::testing::Test {
protected:
    static constexpr const char* kAdmin = "usr_admin";
    static constexpr const char* kWriter = "usr_writer";
    static constexpr const char* kNs = "ns_acme";

    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        db_ = catalog_.db();
        tenant_svc_ = std::make_unique<tenant::TenantService>(db_);
        plan_ = std::make_shared<tenant::UnlimitedPlanProvider>();
        perm_svc_ = std::make_unique<tenant::PermissionService>(db_);
        quota_svc_ = std::make_unique<tenant::QuotaService>(plan_, db_);

        SeedUser(kAdmin, "admin@e.com");
        SeedUser(kWriter, "writer@e.com");

        AuthContext admin_ctx = Ctx(kAdmin);
        auto org = tenant_svc_->CreateOrganization("Acme Org", admin_ctx);
        ASSERT_TRUE(org.ok()) << org.status().message();
        org_id_ = org.value().tenant_id;

        // A namespace owned by kAdmin's tenant (== kAdmin user id) so the ACL
        // owner-admin happy path is reachable.
        SeedNamespace(kNs, kAdmin);

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "tra-admin-key";
        auth_->LoadKeys({MakeKey2(admin_key_, kAdmin, kPermRead | kPermWrite | kPermAdmin)});

        server_ = std::make_unique<httplib::Server>();
        RegisterTenantRoutes(*server_, *tenant_svc_, *perm_svc_, *quota_svc_, *auth_);
        port_ = ArmsPort2(19150);
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
    static AuthContext Ctx(const std::string& uid) {
        AuthContext c;
        c.user_id = uid;
        c.tenant_id = uid;
        c.permissions = kPermRead | kPermWrite | kPermAdmin;
        return c;
    }
    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    static std::string ErrCode(const httplib::Result& r) {
        return json::parse(r->body)["error"]["code"].get<std::string>();
    }

    catalog::CatalogDb catalog_;
    sqlite3* db_ = nullptr;
    std::unique_ptr<tenant::TenantService> tenant_svc_;
    std::shared_ptr<tenant::UnlimitedPlanProvider> plan_;
    std::unique_ptr<tenant::PermissionService> perm_svc_;
    std::unique_ptr<tenant::QuotaService> quota_svc_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_, org_id_;
};

// Grant a tenant+user-scoped ACL (the grantee_user_id body branch), then LIST the ACL
// so AclToJson serializes a populated entry (granted_at / granted_by_user_id / role).
TEST_F(TenantRoutesArms, GrantWithGranteeUserThenListSerializesAcl) {
    httplib::Client cli("127.0.0.1", port_);
    json grant = {{"grantee_tenant_id", kWriter}, {"grantee_user_id", "usr_target"},
                  {"role", "editor"}};
    auto g = cli.Post((std::string("/api/v1/namespaces/") + kNs + "/acl").c_str(), Admin(),
                      grant.dump(), "application/json");
    ASSERT_TRUE(g);
    ASSERT_EQ(g->status, 201) << g->body;

    auto list = cli.Get((std::string("/api/v1/namespaces/") + kNs + "/acl").c_str(), Admin());
    ASSERT_TRUE(list);
    ASSERT_EQ(list->status, 200) << list->body;
    auto body = json::parse(list->body);
    ASSERT_TRUE(body["acl"].is_array());
    ASSERT_GE(body["acl"].size(), 1u);
    const auto& e = body["acl"][0];
    EXPECT_EQ(e["grantee_tenant_id"], kWriter);
    EXPECT_EQ(e["grantee_user_id"], "usr_target");
    EXPECT_EQ(e["role"], "editor");
    EXPECT_TRUE(e.contains("granted_at"));
    EXPECT_TRUE(e.contains("granted_by_user_id"));
}

// PATCH /admin/tenants/{ghost}/quota with a VALID quota_type → passes the body
// validation, then UpdateQuotaOverride fails (tenant not found) → WriteStatusError
// surfaces the service error (the quota update service-error arm).
TEST_F(TenantRoutesArms, UpdateQuotaGhostTenantServiceError) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"quota_type", "max_namespaces"}, {"limit", 50}};
    auto res = cli.Patch("/api/v1/admin/tenants/tenant-ghost/quota", Admin(), b.dump(),
                         "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400) << res->body;
    EXPECT_EQ(ErrCode(res), "CX_ERR_TENANT_NOT_FOUND");
}

// ===========================================================================
// AuthRoutesArms — a recording logger + the leftover auth_routes arms.
// ===========================================================================

// Records the actions a route logs (LogAdminUserAction non-null-logger arm).
class RecordingOplogLogger : public observability::IOperationLogger {
public:
    void Log(const observability::OperationLogEntry& e,
             const observability::TraceContext*) override {
        actions.push_back(e.action);
    }
    void BatchLog(const std::vector<observability::OperationLogEntry>&,
                  const observability::TraceContext*) override {}
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext*) override {
        return observability::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }

    std::vector<std::string> actions;
};

// --- bootstrap POST wrong-token Consume failure --------------------------------
class AuthRoutesArmsBootstrap : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        keys_ = std::make_unique<auth::ApiKeyService>(pdb_.db());
        handler_ = std::make_unique<auth::BootstrapHandler>(pdb_.db(), keys_.get());
        server_ = std::make_unique<httplib::Server>();
        RegisterBootstrapRoutes(*server_, *handler_);
        port_ = ArmsPort2(19170);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        handler_.reset();
        keys_.reset();
        pdb_.Close();
    }
    auth::PlatformDb pdb_;
    std::unique_ptr<auth::ApiKeyService> keys_;
    std::unique_ptr<auth::BootstrapHandler> handler_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
};

// POST with a NON-EMPTY but wrong token → token is not empty (passes the missing-token
// guard) → Consume fails → WriteJsonError surfaces the CX token (the POST Consume-fail
// arm distinct from the missing-token arm).
TEST_F(AuthRoutesArmsBootstrap, PostWrongTokenConsumeFails) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"token", "definitely-not-the-real-token"}};
    auto res = cli.Post("/api/v1/admin/bootstrap", b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400) << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error"));
}

// --- api-key list serializer non-null expires_at -------------------------------
class AuthRoutesArmsApiKeys : public ::testing::Test {
protected:
    static constexpr const char* kOwner = "usr_owner_arms";
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        keys_ = std::make_unique<auth::ApiKeyService>(pdb_.db());
        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "arms-akf-admin";
        auth_->LoadKeys({MakeKey2(admin_key_, kOwner, kPermRead | kPermWrite | kPermAdmin)});
        server_ = std::make_unique<httplib::Server>();
        RegisterApiKeyRoutes(*server_, *keys_, *auth_);
        port_ = ArmsPort2(19190);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        keys_.reset();
        pdb_.Close();
    }
    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    auth::PlatformDb pdb_;
    std::unique_ptr<auth::ApiKeyService> keys_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
};

// Create a key WITH a non-zero expires_at, then list it → ApiKeyInfoToJson renders a
// non-null expires_at (the != 0 serializer branch the create-only test never lists).
TEST_F(AuthRoutesArmsApiKeys, ListRendersNonNullExpiresAt) {
    httplib::Client cli("127.0.0.1", port_);
    json create = {{"name", "ttl-key"}, {"expires_at", 7777777777777LL}};
    auto c = cli.Post("/api/v1/auth/api-keys", Admin(), create.dump(), "application/json");
    ASSERT_TRUE(c);
    ASSERT_EQ(c->status, 200) << c->body;

    auto list = cli.Get("/api/v1/auth/api-keys", Admin());
    ASSERT_TRUE(list);
    ASSERT_EQ(list->status, 200) << list->body;
    auto arr = json::parse(list->body);
    ASSERT_TRUE(arr.is_array());
    ASSERT_GE(arr.size(), 1u);
    EXPECT_FALSE(arr[0]["expires_at"].is_null());
    EXPECT_EQ(arr[0]["expires_at"], 7777777777777LL);
}

// --- admin/users with a real logger + PATCH malformed JSON ---------------------
class AuthRoutesArmsAdminUsers : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        hasher_ = std::make_unique<auth::Pbkdf2PasswordHasher>(/*iterations=*/1000);
        users_ = std::make_unique<auth::AdminUsersService>(pdb_.db(), hasher_.get());
        logger_ = std::make_unique<RecordingOplogLogger>();

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "arms-au-admin";
        auth_->LoadKeys({MakeKey2(admin_key_, "admin-user", kPermRead | kPermWrite | kPermAdmin)});

        server_ = std::make_unique<httplib::Server>();
        RegisterAdminUsersRoutes(*server_, *users_, *auth_, logger_.get());
        port_ = ArmsPort2(19210);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        users_.reset();
        pdb_.Close();
    }
    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    std::string CreateUser(const std::string& email) {
        httplib::Client cli("127.0.0.1", port_);
        json b = {{"email", email}, {"password", "Password123"}, {"role", "user"}};
        auto r = cli.Post("/api/v1/admin/users", Admin(), b.dump(), "application/json");
        EXPECT_TRUE(r);
        EXPECT_EQ(r->status, 200) << (r ? r->body : "no-response");
        return r ? json::parse(r->body)["user"]["id"].get<std::string>() : "";
    }

    auth::PlatformDb pdb_;
    std::unique_ptr<auth::Pbkdf2PasswordHasher> hasher_;
    std::unique_ptr<auth::AdminUsersService> users_;
    std::unique_ptr<RecordingOplogLogger> logger_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
};

// Create + disable + enable WITH a real logger → LogAdminUserAction runs (non-null
// logger arm) and records user.created / user.disabled / user.enabled.
TEST_F(AuthRoutesArmsAdminUsers, MutationsAreLoggedViaRealLogger) {
    const std::string id = CreateUser("logged@example.com");
    ASSERT_FALSE(id.empty());
    httplib::Client cli("127.0.0.1", port_);
    auto dis = cli.Post("/api/v1/admin/users/" + id + "/disable", Admin(), std::string(),
                        "application/json");
    ASSERT_TRUE(dis);
    ASSERT_EQ(dis->status, 200) << dis->body;
    auto en = cli.Post("/api/v1/admin/users/" + id + "/enable", Admin(), std::string(),
                       "application/json");
    ASSERT_TRUE(en);
    ASSERT_EQ(en->status, 200) << en->body;

    // The logger captured all three mutations (proves the non-null-logger arm ran).
    EXPECT_NE(std::find(logger_->actions.begin(), logger_->actions.end(), "user.created"),
              logger_->actions.end());
    EXPECT_NE(std::find(logger_->actions.begin(), logger_->actions.end(), "user.disabled"),
              logger_->actions.end());
    EXPECT_NE(std::find(logger_->actions.begin(), logger_->actions.end(), "user.enabled"),
              logger_->actions.end());
}

// PATCH /admin/users/:id with malformed JSON → 422 kUserValidation (the PATCH parse
// catch arm, distinct from the POST parse arm the admin-users suite covers).
TEST_F(AuthRoutesArmsAdminUsers, PatchMalformedJsonIs422) {
    const std::string id = CreateUser("patchme@example.com");
    ASSERT_FALSE(id.empty());
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch("/api/v1/admin/users/" + id, Admin(), "not json{", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 422) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_VALIDATION");
}

}  // namespace
}  // namespace cortrix
