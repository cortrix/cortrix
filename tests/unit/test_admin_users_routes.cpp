#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
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
#include "cortrix/server/routes/auth_routes.h"

// FA1 R11 coverage: the auth admin/users 5 endpoints over a real httplib
// server backed by an in-memory platform.db (migrated with the auth schema) and a
// real ApiKeyAuth (admin key + a read-only non-admin key). Exercises list / create
// / update / disable / enable + the admin permission gate + 404 / 409 / 422 errors.
namespace cortrix {
namespace {

using json = nlohmann::json;

class AdminUsersRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(platform_db_.Open(":memory:").ok());
        // Fast PBKDF2 for tests (the default 600k iterations is too slow per call).
        hasher_ = std::make_unique<auth::Pbkdf2PasswordHasher>(/*iterations=*/1000);
        service_ = std::make_unique<auth::AdminUsersService>(platform_db_.db(), hasher_.get());

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "admin-users-admin-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "admin-user";
        admin_kc.permissions = kPermRead | kPermWrite | kPermAdmin;

        user_key_ = "admin-users-readonly-key";
        ApiKeyConfig user_kc;
        user_kc.key_hash = ApiKeyAuth::HashKey(user_key_);
        user_kc.tenant_id = "alice";
        user_kc.permissions = kPermRead;
        auth_->LoadKeys({admin_kc, user_kc});

        server_ = std::make_unique<httplib::Server>();
        // logger == nullptr: the operation_log write is a no-op (covered separately).
        RegisterAdminUsersRoutes(*server_, *service_, *auth_, /*logger=*/nullptr);
        port_ = 19300 + (getpid() % 400);
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

    // Create a user via the service (bypassing HTTP) and return its id.
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

// ---- create + list -----------------------------------------------------------

TEST_F(AdminUsersRoutesTest, CreateThenListReturnsUser) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"email", "bob@example.com"}, {"password", "Password123"}, {"role", "user"}};
    auto res = cli.Post("/api/v1/admin/users", Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("user"));
    EXPECT_EQ(body["user"]["email"], "bob@example.com");
    EXPECT_EQ(body["user"]["role"], "user");
    EXPECT_EQ(body["user"]["status"], "active");
    EXPECT_EQ(body["user"]["email_verified"], true);  // invite mode → verified
    EXPECT_FALSE(body["user"]["id"].get<std::string>().empty());

    auto list = cli.Get("/api/v1/admin/users", Admin());
    ASSERT_TRUE(list);
    ASSERT_EQ(list->status, 200) << list->body;
    auto lbody = json::parse(list->body);
    EXPECT_EQ(lbody["total"], 1);
    EXPECT_EQ(lbody["page"], 1);
    EXPECT_EQ(lbody["limit"], 20);
    ASSERT_EQ(lbody["users"].size(), 1u);
    EXPECT_EQ(lbody["users"][0]["email"], "bob@example.com");
}

TEST_F(AdminUsersRoutesTest, ListFiltersByRoleStatusAndQuery) {
    SeedUser("admin1@example.com", "admin");
    SeedUser("carol@example.com", "user");
    const std::string did = SeedUser("dave@example.com", "user");
    service_->Disable(did);

    httplib::Client cli("127.0.0.1", port_);
    // role=admin → 1
    auto a = cli.Get("/api/v1/admin/users?role=admin", Admin());
    ASSERT_TRUE(a);
    EXPECT_EQ(json::parse(a->body)["total"], 1);
    // status=disabled → 1 (dave)
    auto d = cli.Get("/api/v1/admin/users?status=disabled", Admin());
    ASSERT_TRUE(d);
    auto dbody = json::parse(d->body);
    EXPECT_EQ(dbody["total"], 1);
    EXPECT_EQ(dbody["users"][0]["email"], "dave@example.com");
    // status=active → 2 (admin1 + carol; dave is disabled)
    auto act = cli.Get("/api/v1/admin/users?status=active", Admin());
    ASSERT_TRUE(act);
    EXPECT_EQ(json::parse(act->body)["total"], 2);
    // q=carol → 1
    auto q = cli.Get("/api/v1/admin/users?q=carol", Admin());
    ASSERT_TRUE(q);
    EXPECT_EQ(json::parse(q->body)["total"], 1);
}

// ---- update ------------------------------------------------------------------

TEST_F(AdminUsersRoutesTest, UpdateChangesRoleAndDisplayName) {
    const std::string id = SeedUser("eve@example.com", "user");
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"display_name", "Eve Smith"}, {"role", "admin"}, {"email_verified", true}};
    auto res = cli.Patch("/api/v1/admin/users/" + id, Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["user"]["display_name"], "Eve Smith");
    EXPECT_EQ(body["user"]["role"], "admin");
    EXPECT_EQ(body["user"]["email_verified"], true);
}

// ---- disable + enable --------------------------------------------------------

TEST_F(AdminUsersRoutesTest, DisableThenEnableTogglesStatus) {
    const std::string id = SeedUser("frank@example.com", "user");
    httplib::Client cli("127.0.0.1", port_);

    auto dis = cli.Post("/api/v1/admin/users/" + id + "/disable", Admin(),
                        std::string(), "application/json");
    ASSERT_TRUE(dis);
    ASSERT_EQ(dis->status, 200) << dis->body;
    EXPECT_EQ(json::parse(dis->body)["user"]["status"], "disabled");

    auto en = cli.Post("/api/v1/admin/users/" + id + "/enable", Admin(),
                       std::string(), "application/json");
    ASSERT_TRUE(en);
    ASSERT_EQ(en->status, 200) << en->body;
    EXPECT_EQ(json::parse(en->body)["user"]["status"], "active");
}

// ---- permission gate ---------------------------------------------------------

TEST_F(AdminUsersRoutesTest, NonAdminListIsForbidden) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/admin/users", User());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
}

TEST_F(AdminUsersRoutesTest, NoAuthIsUnauthenticated) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/admin/users");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ---- error bodies (404 / 409 / 422) --------------------------------

TEST_F(AdminUsersRoutesTest, UpdateMissingUserIs404) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"role", "admin"}};
    auto res = cli.Patch("/api/v1/admin/users/usr_doesnotexist", Admin(),
                         req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_USER_NOT_FOUND");
}

TEST_F(AdminUsersRoutesTest, DisableMissingUserIs404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/admin/users/usr_nope/disable", Admin(),
                        std::string(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_NOT_FOUND");
}

TEST_F(AdminUsersRoutesTest, DuplicateEmailIs409) {
    SeedUser("dup@example.com", "user");
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"email", "dup@example.com"}, {"password", "Password123"}, {"role", "user"}};
    auto res = cli.Post("/api/v1/admin/users", Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 409) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_EMAIL_EXISTS");
}

TEST_F(AdminUsersRoutesTest, BadEmailIs422) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"email", "not-an-email"}, {"password", "Password123"}, {"role", "user"}};
    auto res = cli.Post("/api/v1/admin/users", Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 422) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_VALIDATION");
}

TEST_F(AdminUsersRoutesTest, BadRoleIs422) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"email", "good@example.com"}, {"password", "Password123"}, {"role", "superuser"}};
    auto res = cli.Post("/api/v1/admin/users", Admin(), req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 422) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_USER_VALIDATION");
}

}  // namespace
}  // namespace cortrix
