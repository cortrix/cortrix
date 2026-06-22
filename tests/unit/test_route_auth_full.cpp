// auth_routes.cpp boundary coverage (the route module is ~49% line). Targets the
// endpoints NOT already exercised by test_jwt_rotate_route.cpp / test_admin_users_*:
//   * RegisterBootstrapRoutes   GET + POST /api/v1/admin/bootstrap
//   * RegisterApiKeyRoutes      POST / GET / DELETE /api/v1/auth/api-keys
//   * RegisterAuthSessionRoute  GET /api/v1/auth/me  (auth-off + auth-on arms)
// plus the MakeAuthError arms WriteAuthError funnels (kBootstrapTokenInvalid /
// kInvalidRequest), and the validation 400s.
//
// In-process loopback harness (httplib::Server + the Register* entry points) over a
// real in-memory platform.db (PlatformDb). The config admin key's tenant_id is set
// to the seeded user id so the config-auth path yields user_id == that id (api keys
// are owned by users.id). Suite names AuthRouteFull* are globally unique. ApiKeyAuth
// is non-copyable → built in place, passed by reference. No LLM / no real network.
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
#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/bootstrap_handler.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/server/routes/auth_routes.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using auth::ApiKeyService;
using auth::BootstrapHandler;
using auth::PlatformDb;

int UniquePort(int base) {
    static std::atomic<unsigned> ctr{0};
    return base + ((getpid() + ctr.fetch_add(1)) % 200);
}

// ===========================================================================
// Bootstrap routes (GET + POST /api/v1/admin/bootstrap). NOT WithAuth-wrapped:
// the bootstrap token IS the credential on first run.
// ===========================================================================
class AuthRouteFullBootstrap : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        keys_ = std::make_unique<ApiKeyService>(pdb_.db());
        handler_ = std::make_unique<BootstrapHandler>(pdb_.db(), keys_.get());

        server_ = std::make_unique<httplib::Server>();
        RegisterBootstrapRoutes(*server_, *handler_);
        port_ = UniquePort(19880);
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

    PlatformDb pdb_;
    std::unique_ptr<ApiKeyService> keys_;
    std::unique_ptr<BootstrapHandler> handler_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
};

// GET with no token → kBootstrapTokenInvalid (401 auth envelope).
TEST_F(AuthRouteFullBootstrap, GetMissingTokenIsBootstrapInvalid) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/admin/bootstrap");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"],
              "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID");
}

// GET with a wrong token → Consume fails → WriteJsonError surfaces the CX token.
TEST_F(AuthRouteFullBootstrap, GetWrongTokenIsInvalid) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/admin/bootstrap?token=not-the-token");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"],
              "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID");
}

// POST with no token in body → kBootstrapTokenInvalid (the missing-token branch).
TEST_F(AuthRouteFullBootstrap, PostMissingTokenIsBootstrapInvalid) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/admin/bootstrap", "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"],
              "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID");
}

// POST with malformed JSON → still routed to the missing-token error (the parse
// exception is swallowed and token stays empty).
TEST_F(AuthRouteFullBootstrap, PostMalformedJsonIsBootstrapInvalid) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/admin/bootstrap", "{not json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"],
              "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID");
}

// GET with the REAL token → 200 one-time HTML (the success render path).
TEST_F(AuthRouteFullBootstrap, GetValidTokenRendersHtml) {
    auto tok = handler_->GenerateToken();
    ASSERT_TRUE(tok.ok());
    ASSERT_FALSE(tok.value().empty());  // first start → a token exists
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(("/api/v1/admin/bootstrap?token=" + tok.value()).c_str());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_NE(res->get_header_value("Content-Type").find("text/html"), std::string::npos);
}

// POST with the REAL token → 200 JSON body carrying the admin key.
TEST_F(AuthRouteFullBootstrap, PostValidTokenRendersJson) {
    auto tok = handler_->GenerateToken();
    ASSERT_TRUE(tok.ok());
    ASSERT_FALSE(tok.value().empty());
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"token", tok.value()}};
    auto res = cli.Post("/api/v1/admin/bootstrap", b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    // The JSON render path returns the one-time admin key.
    auto body = json::parse(res->body);
    EXPECT_TRUE(body.is_object());
}

// ===========================================================================
// API-key routes (POST / GET / DELETE /api/v1/auth/api-keys). WithAuth(kPermAdmin).
// The config admin key's tenant_id == the seeded user id, so the authenticated
// user_id owns the keys it creates.
// ===========================================================================
class AuthRouteFullApiKeys : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        SeedUser(kOwner);
        keys_ = std::make_unique<ApiKeyService>(pdb_.db());

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "akf-admin-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = kOwner;  // → user_id == kOwner (config-auth path)
        admin_kc.permissions = kPermRead | kPermWrite | kPermAdmin;

        user_key_ = "akf-readonly-key";
        ApiKeyConfig user_kc;
        user_kc.key_hash = ApiKeyAuth::HashKey(user_key_);
        user_kc.tenant_id = "alice";
        user_kc.permissions = kPermRead;  // no admin
        auth_->LoadKeys({admin_kc, user_kc});

        server_ = std::make_unique<httplib::Server>();
        RegisterApiKeyRoutes(*server_, *keys_, *auth_);
        port_ = UniquePort(19920);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        keys_.reset();
        pdb_.Close();
    }

    void SeedUser(const std::string& id) {
        const std::string sql =
            "INSERT INTO users(id,email,password_hash,display_name,email_verified,"
            "status,login_attempts,created_at,updated_at) VALUES('" + id +
            "','o@e.com','h','O',1,'active',0,1,1)";
        ASSERT_EQ(sqlite3_exec(pdb_.db(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers User() { return {{"Authorization", "Bearer " + user_key_}}; }

    static constexpr const char* kOwner = "usr_owner1";
    PlatformDb pdb_;
    std::unique_ptr<ApiKeyService> keys_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_, user_key_;
};

// POST no auth → 401.
TEST_F(AuthRouteFullApiKeys, CreateNoAuth401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/auth/api-keys", R"({"name":"k"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// POST with a non-admin key → 403.
TEST_F(AuthRouteFullApiKeys, CreateNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/auth/api-keys", User(), R"({"name":"k"})",
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// POST with malformed JSON → kInvalidRequest (400).
TEST_F(AuthRouteFullApiKeys, CreateMalformedJsonIsInvalidRequest) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/auth/api-keys", Admin(), "{not json",
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_INVALID_REQUEST");
}

// POST missing required 'name' → kInvalidRequest (400).
TEST_F(AuthRouteFullApiKeys, CreateMissingNameIsInvalidRequest) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/auth/api-keys", Admin(), R"({"foo":"bar"})",
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_INVALID_REQUEST");
}

// POST happy path → 200, plaintext key returned ONCE. Then GET lists it (prefix
// only), then DELETE revokes it.
TEST_F(AuthRouteFullApiKeys, CreateListDeleteRoundTrip) {
    httplib::Client cli("127.0.0.1", port_);
    auto created = cli.Post("/api/v1/auth/api-keys", Admin(),
                            R"({"name":"my-agent"})", "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 200) << created->body;
    auto cbody = json::parse(created->body);
    ASSERT_TRUE(cbody.contains("key"));
    EXPECT_EQ(cbody["key"].get<std::string>().rfind("cortrix_sk_", 0), 0u);
    const std::string key_id = cbody["id"];

    // GET lists the key (never the plaintext).
    auto listed = cli.Get("/api/v1/auth/api-keys", Admin());
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->status, 200) << listed->body;
    auto larr = json::parse(listed->body);
    ASSERT_TRUE(larr.is_array());
    ASSERT_GE(larr.size(), 1u);
    EXPECT_FALSE(larr[0].contains("key"));  // plaintext never re-listed
    EXPECT_TRUE(larr[0].contains("key_prefix"));

    // DELETE revokes it.
    auto del = cli.Delete(("/api/v1/auth/api-keys/" + key_id).c_str(), Admin());
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 200) << del->body;
    EXPECT_EQ(json::parse(del->body)["revoked"], true);
}

// POST with expires_at as an integer epoch-ms → accepted (the expires_at branch).
TEST_F(AuthRouteFullApiKeys, CreateWithExpiresAtInteger) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"name", "ttl-agent"}, {"expires_at", 9999999999999LL}};
    auto res = cli.Post("/api/v1/auth/api-keys", Admin(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_FALSE(body["expires_at"].is_null());
    EXPECT_EQ(body["expires_at"], 9999999999999LL);
}

// DELETE a nonexistent key id → CX_ERR_NOT_FOUND (404).
TEST_F(AuthRouteFullApiKeys, DeleteNonexistentIsNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/auth/api-keys/key_ghost", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
}

// ===========================================================================
// Auth session probe GET /api/v1/auth/me (NOT WithAuth-wrapped).
// ===========================================================================
class AuthRouteFullSession : public ::testing::Test {
protected:
    void Start(bool auth_enabled) {
        auth_ = std::make_unique<ApiKeyAuth>();
        auth_->set_enabled(auth_enabled);
        server_ = std::make_unique<httplib::Server>();
        RegisterAuthSessionRoute(*server_, *auth_);
        port_ = UniquePort(19960);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
};

// auth DISABLED → 200 synthetic single-operator identity.
TEST_F(AuthRouteFullSession, AuthDisabledReturnsOperatorIdentity) {
    Start(/*auth_enabled=*/false);
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/auth/me");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["id"], "operator");
    EXPECT_EQ(body["role"], "admin");
    EXPECT_EQ(body["auth_disabled"], true);
    EXPECT_EQ(body["edition"], "ce");
}

// auth ENABLED → 401 (UI shows login; the kUnauthorized MakeAuthError arm).
TEST_F(AuthRouteFullSession, AuthEnabledReturns401) {
    Start(/*auth_enabled=*/true);
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/auth/me");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_UNAUTHORIZED");
}

}  // namespace
}  // namespace cortrix
