// Route request-validation matrices for the admin-area routes:
//   - P08 admin/users (POST/GET/PATCH /api/v1/admin/users ...)
//   - OPEN-2 GC + maintenance ops (/api/v1/gc/*, /api/v1/maintenance/*)
//
// BREADTH coverage of: missing auth -> 401, insufficient perm (non-admin) ->
// 403, malformed JSON -> 400, missing/empty required fields -> 4xx, bad enums
// -> 422, not-found -> 404 (CX_ERR_USER_NOT_FOUND / CX_ERR_GC_*), conflict ->
// 409, X-Ops-Confirm gate -> 400, and method-not-allowed / unknown route ->
// generic 404 (anti-clobber).
//
// Same in-process harnesses as test_admin_users_routes.cpp + test_gc_routes.cpp.
// Suite/fixture names area-prefixed (AdminUsersRouteValMatrix / GcRouteValMatrix)
// for global uniqueness. No live network / LLM.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/admin_users_service.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/auth/pbkdf2_password_hasher.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/common/data_types.h"
#include "cortrix/config/config.h"
#include "cortrix/server/routes/auth_routes.h"
#include "cortrix/server/routes/gc_routes.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// ===========================================================================
// admin/users route validation
// ===========================================================================
class AdminUsersRouteValMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(platform_db_.Open(":memory:").ok());
        hasher_ = std::make_unique<auth::Pbkdf2PasswordHasher>(/*iterations=*/1000);
        service_ = std::make_unique<auth::AdminUsersService>(platform_db_.db(), hasher_.get());

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "auvm-admin-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "admin-user";
        admin_kc.permissions = kPermRead | kPermWrite | kPermAdmin;

        user_key_ = "auvm-readonly-key";
        ApiKeyConfig user_kc;
        user_kc.key_hash = ApiKeyAuth::HashKey(user_key_);
        user_kc.tenant_id = "alice";
        user_kc.permissions = kPermRead;
        auth_->LoadKeys({admin_kc, user_kc});

        server_ = std::make_unique<httplib::Server>();
        RegisterAdminUsersRoutes(*server_, *service_, *auth_, /*logger=*/nullptr);
        port_ = 18550 + (getpid() % 250);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        service_.reset();
        platform_db_.Close();
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers User() { return {{"Authorization", "Bearer " + user_key_}}; }

    std::string SeedUser(const std::string& email, const std::string& role) {
        auto r = service_->Create(email, "Password123", role, /*display_name=*/email);
        EXPECT_TRUE(r.ok()) << r.status().message();
        return r.ok() ? r.value().id : "";
    }

    auth::PlatformDb platform_db_;
    std::unique_ptr<auth::Pbkdf2PasswordHasher> hasher_;
    std::unique_ptr<auth::AdminUsersService> service_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
    std::string user_key_;
};

// ---- auth / perm gate over all 5 endpoints (TEST_P) -----------------------
// (method, path) tuples; each must 401 with no auth and 403 with a read-only key.

struct AdminEndpoint {
    std::string method;
    std::string path;
};

class AdminUsersRouteValAuthGate
    : public AdminUsersRouteValMatrix,
      public ::testing::WithParamInterface<AdminEndpoint> {
protected:
    httplib::Result Call(httplib::Client& cli, const httplib::Headers& h) {
        const auto& e = GetParam();
        if (e.method == "GET") return cli.Get(e.path.c_str(), h);
        if (e.method == "POST") return cli.Post(e.path.c_str(), h, "{}", "application/json");
        if (e.method == "PATCH") return cli.Patch(e.path.c_str(), h, "{}", "application/json");
        return cli.Get(e.path.c_str(), h);
    }
};

TEST_P(AdminUsersRouteValAuthGate, NoAuthIs401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = Call(cli, {});
    ASSERT_TRUE(res) << GetParam().method << " " << GetParam().path;
    EXPECT_EQ(res->status, 401) << GetParam().method << " " << GetParam().path;
}

TEST_P(AdminUsersRouteValAuthGate, NonAdminIs403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = Call(cli, User());
    ASSERT_TRUE(res) << GetParam().method << " " << GetParam().path;
    EXPECT_EQ(res->status, 403) << GetParam().method << " " << GetParam().path
                                << " body=" << res->body;
}

INSTANTIATE_TEST_SUITE_P(
    Endpoints, AdminUsersRouteValAuthGate,
    ::testing::Values(
        AdminEndpoint{"GET", "/api/v1/admin/users"},
        AdminEndpoint{"POST", "/api/v1/admin/users"},
        AdminEndpoint{"PATCH", "/api/v1/admin/users/usr_x"},
        AdminEndpoint{"POST", "/api/v1/admin/users/usr_x/disable"},
        AdminEndpoint{"POST", "/api/v1/admin/users/usr_x/enable"}));

// ---- malformed JSON on create ---------------------------------------------

class AdminUsersRouteValBadJson
    : public AdminUsersRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(AdminUsersRouteValBadJson, Create400Or422) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/admin/users", Admin(), GetParam(), "application/json");
    ASSERT_TRUE(res) << GetParam();
    // Malformed JSON -> 400; an empty-but-valid object -> 422 (missing fields).
    EXPECT_TRUE(res->status == 400 || res->status == 422)
        << GetParam() << " status=" << res->status;
}

INSTANTIATE_TEST_SUITE_P(
    Bodies, AdminUsersRouteValBadJson,
    ::testing::Values(std::string("{not json"), std::string("}{"),
                      std::string("[1,2"), std::string("\"str\""),
                      std::string("null"), std::string("{\"email\":}"),
                      std::string("")));

// ---- missing / empty required fields on create (422 validation) -----------

TEST_F(AdminUsersRouteValMatrix, CreateMissingEmail422) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"password", "Password123"}, {"role", "user"}};
    auto res = cli.Post("/api/v1/admin/users", Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 422) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_VALIDATION");
}

TEST_F(AdminUsersRouteValMatrix, CreateEmptyEmail422) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"email", ""}, {"password", "Password123"}, {"role", "user"}};
    auto res = cli.Post("/api/v1/admin/users", Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 422) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_VALIDATION");
}

// ---- bad email / role enum matrix (TEST_P) --------------------------------

struct AdminCreateBad {
    std::string email;
    std::string role;
};

class AdminUsersRouteValBadField
    : public AdminUsersRouteValMatrix,
      public ::testing::WithParamInterface<AdminCreateBad> {};

TEST_P(AdminUsersRouteValBadField, Validation422) {
    const auto& tc = GetParam();
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"email", tc.email}, {"password", "Password123"}, {"role", tc.role}};
    auto res = cli.Post("/api/v1/admin/users", Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res) << tc.email << "/" << tc.role;
    EXPECT_EQ(res->status, 422) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_VALIDATION");
}

INSTANTIATE_TEST_SUITE_P(
    Fields, AdminUsersRouteValBadField,
    ::testing::Values(
        AdminCreateBad{"not-an-email", "user"},
        AdminCreateBad{"missing-at-sign.com", "user"},
        AdminCreateBad{"a@b@c.com", "user"},
        AdminCreateBad{"spaces in@email.com", "user"},
        AdminCreateBad{"@nolocal.com", "user"},
        AdminCreateBad{"nodomain@", "user"},
        AdminCreateBad{"plain", "user"},
        AdminCreateBad{"good@example.com", "superuser"},
        AdminCreateBad{"good2@example.com", "root"},
        AdminCreateBad{"good3@example.com", ""},
        AdminCreateBad{"good4@example.com", "ADMIN_X"},
        AdminCreateBad{"good5@example.com", "User"},
        AdminCreateBad{"good6@example.com", "Admin"},
        AdminCreateBad{"good7@example.com", "guest"}));

// ---- not-found (404) on update / disable / enable -------------------------

class AdminUsersRouteValMissingUser
    : public AdminUsersRouteValMatrix,
      public ::testing::WithParamInterface<AdminEndpoint> {
protected:
    httplib::Result Call(httplib::Client& cli) {
        const auto& e = GetParam();
        if (e.method == "PATCH")
            return cli.Patch(e.path.c_str(), Admin(), R"({"role":"admin"})", "application/json");
        return cli.Post(e.path.c_str(), Admin(), std::string(), "application/json");
    }
};

TEST_P(AdminUsersRouteValMissingUser, Returns404UserNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = Call(cli);
    ASSERT_TRUE(res) << GetParam().method << " " << GetParam().path;
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_NOT_FOUND");
}

INSTANTIATE_TEST_SUITE_P(
    MissingUser, AdminUsersRouteValMissingUser,
    ::testing::Values(
        AdminEndpoint{"PATCH", "/api/v1/admin/users/usr_ghost"},
        AdminEndpoint{"POST", "/api/v1/admin/users/usr_ghost/disable"},
        AdminEndpoint{"POST", "/api/v1/admin/users/usr_ghost/enable"}));

// ---- conflict (409) duplicate email ---------------------------------------

TEST_F(AdminUsersRouteValMatrix, DuplicateEmail409) {
    SeedUser("dup@example.com", "user");
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"email", "dup@example.com"}, {"password", "Password123"}, {"role", "user"}};
    auto res = cli.Post("/api/v1/admin/users", Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 409) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_EMAIL_EXISTS");
}

// ---- method-not-allowed / unknown route -----------------------------------

TEST_F(AdminUsersRouteValMatrix, DeleteOnUsersGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/admin/users", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(AdminUsersRouteValMatrix, UnknownAdminRouteGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/admin/nonexistent", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

// ===========================================================================
// GC + maintenance ops route validation
// ===========================================================================
class GcRouteValMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);

        gc_ = std::make_unique<catalog::gc::GcManager>(catalog_.db(), &sink_, cfg_);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("gcval_" + std::to_string(::getpid())));
        harness_->Admit(kNs);
        using ::testing::_;
        using ::testing::Invoke;
        ON_CALL(router_, ListNamespaces(_))
            .WillByDefault(Invoke([](const catalog::ListNamespacesOptions&) {
                catalog::BatchResult<std::string> r;
                r.results = {kNs};
                return cortrix::Result<catalog::BatchResult<std::string>>(r);
            }));
        sweeper_ = std::make_unique<catalog::gc::DocumentGcSweeper>(
            &router_, &harness_->ipool(), cfg_);

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "gcval-admin-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "admin-user";
        admin_kc.permissions = kPermRead | kPermWrite | kPermAdmin;

        user_key_ = "gcval-user-key";
        ApiKeyConfig user_kc;
        user_kc.key_hash = ApiKeyAuth::HashKey(user_key_);
        user_kc.tenant_id = "alice";
        user_kc.permissions = kPermRead | kPermWrite;  // no admin
        auth_->LoadKeys({admin_kc, user_kc});

        server_ = std::make_unique<httplib::Server>();
        RegisterGcRoutes(*server_, *gc_, *sweeper_, *auth_);
        port_ = 18820 + (getpid() % 250);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers AdminConfirm() {
        return {{"Authorization", "Bearer " + admin_key_}, {"X-Ops-Confirm", "true"}};
    }
    httplib::Headers User() { return {{"Authorization", "Bearer " + user_key_}}; }

    static constexpr const char* kNs = "sales";
    catalog::gc::NullBlobGcSink sink_;
    catalog::CatalogDb catalog_;
    GcConfig cfg_;
    std::unique_ptr<catalog::gc::GcManager> gc_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> router_;
    std::unique_ptr<catalog::gc::DocumentGcSweeper> sweeper_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
    std::string user_key_;
};

// ---- auth gate over all GC + maintenance endpoints (TEST_P) ----------------

struct GcEndpoint {
    std::string method;
    std::string path;
};

class GcRouteValAuthGate
    : public GcRouteValMatrix,
      public ::testing::WithParamInterface<GcEndpoint> {
protected:
    httplib::Result Call(httplib::Client& cli, const httplib::Headers& h) {
        const auto& e = GetParam();
        if (e.method == "GET") return cli.Get(e.path.c_str(), h);
        return cli.Post(e.path.c_str(), h, "", "application/json");
    }
};

TEST_P(GcRouteValAuthGate, NoAuth401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = Call(cli, {});
    ASSERT_TRUE(res) << GetParam().method << " " << GetParam().path;
    EXPECT_EQ(res->status, 401) << GetParam().method << " " << GetParam().path;
}

TEST_P(GcRouteValAuthGate, NonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = Call(cli, User());
    ASSERT_TRUE(res) << GetParam().method << " " << GetParam().path;
    EXPECT_EQ(res->status, 403) << GetParam().method << " " << GetParam().path
                                << " body=" << res->body;
}

INSTANTIATE_TEST_SUITE_P(
    Endpoints, GcRouteValAuthGate,
    ::testing::Values(
        GcEndpoint{"GET", "/api/v1/gc/status"},
        GcEndpoint{"POST", "/api/v1/gc/run"},
        GcEndpoint{"POST", "/api/v1/gc/restore"},
        GcEndpoint{"POST", "/api/v1/gc/purge"},
        GcEndpoint{"POST", "/api/v1/maintenance/vacuum"},
        GcEndpoint{"POST", "/api/v1/maintenance/reindex"}));

// ---- X-Ops-Confirm gate on destructive ops (TEST_P) -----------------------

class GcRouteValConfirmGate
    : public GcRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(GcRouteValConfirmGate, MissingConfirm400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post(GetParam().c_str(), Admin(), "", "application/json");
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 400) << GetParam() << " body=" << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_OPS_CONFIRM_REQUIRED");
}

INSTANTIATE_TEST_SUITE_P(Destructive, GcRouteValConfirmGate,
                         ::testing::Values(std::string("/api/v1/gc/run"),
                                           std::string("/api/v1/gc/purge")));

// ---- restore body validation ----------------------------------------------

TEST_F(GcRouteValMatrix, RestoreMalformedJson400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/restore", Admin(), "{not json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400);
    EXPECT_LT(res->status, 500);
}

TEST_F(GcRouteValMatrix, RestoreMissingDocumentIdsAccepted) {
    // Real behavior: a restore body with no document_ids is a no-op success (it
    // restores nothing), NOT a validation error. Pin the actual 2xx contract.
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/restore", Admin(), "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 200);
    EXPECT_LT(res->status, 300);
}

// ---- maintenance happy path (negative control for the confirm gate) -------

TEST_F(GcRouteValMatrix, VacuumNoConfirmAccepted202) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/maintenance/vacuum", Admin(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 202) << res->body;
}

TEST_F(GcRouteValMatrix, RunWithConfirmAccepted202) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/run", AdminConfirm(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 202) << res->body;
}

// ---- method-not-allowed / unknown route -----------------------------------

TEST_F(GcRouteValMatrix, PostOnGcStatusGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/status", Admin(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(GcRouteValMatrix, UnknownGcRouteGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/gc/nonexistent", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
}  // namespace cortrix
