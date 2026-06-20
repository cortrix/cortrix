// Error-path coverage for previously-untested Auth CX_ERR_* codes.
//
// Each TEST genuinely constructs the precondition that makes the code fire and
// asserts on the surfaced identity (the AuthErrorCode -> CX_ERR_* token, the
// JSON error-envelope "code" field, and/or the HTTP status), not just a string
// reference.
//
// Covered:
//   - CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID  (BootstrapHandler::Consume, all 3 reasons)
//   - CX_ERR_AUTH_ADMIN_REQUIRED           (system_config PUT, non-admin caller)
//
// Deferred (see the test/report): CX_ERR_AUTH_BCRYPT_TIMEOUT has no firing site
// in the Phase-1 codebase.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <sqlite3.h>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/bootstrap_handler.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/server/routes/system_config_routes.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using auth::ApiKeyService;
using auth::AuthErrorCode;
using auth::AuthErrorCodeString;
using auth::BootstrapHandler;
using auth::PlatformDb;

// ===========================================================================
// CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID -- service layer (BootstrapHandler).
//
// Consume() validates the token under the lock BEFORE any DB work and returns
// AuthStatus(kBootstrapTokenInvalid, "reason=...") for the not_found / used /
// expired cases. The CX_ERR_* token prefixes the Status message
// ("CX_ERR_X: detail"); we assert that prefix.
// ===========================================================================
class BootstrapErrPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        api_keys_ = std::make_unique<ApiKeyService>(pdb_.db());
        handler_ = std::make_unique<BootstrapHandler>(pdb_.db(), api_keys_.get());
    }

    static bool HasCx(const Status& st, const char* cx) {
        return !st.ok() && st.message().rfind(cx, 0) == 0;  // starts_with
    }

    PlatformDb pdb_;
    std::unique_ptr<ApiKeyService> api_keys_;
    std::unique_ptr<BootstrapHandler> handler_;
};

// reason=not_found: no token was ever generated, so the stored token is empty
// and any presented token is rejected as not_found.
TEST_F(BootstrapErrPathTest, ConsumeWithNoActiveTokenIsBootstrapInvalid) {
    auto r = handler_->Consume("ctx_bootstrap_does_not_exist");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCx(r.status(), "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID"))
        << r.status().message();
    EXPECT_NE(r.status().message().find("reason=not_found"), std::string::npos)
        << r.status().message();
}

// reason=not_found: a token IS active, but a different value is presented.
TEST_F(BootstrapErrPathTest, ConsumeWrongTokenIsBootstrapInvalid) {
    auto gen = handler_->GenerateToken();
    ASSERT_TRUE(gen.ok()) << gen.status().message();
    ASSERT_FALSE(gen.value().empty());  // empty platform.db -> first start -> token minted

    auto r = handler_->Consume(gen.value() + "_tampered");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCx(r.status(), "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID"))
        << r.status().message();
}

// reason=used: a successful consume marks the token used; a second consume of
// the same token is rejected (single-use, V3 resolution 5).
TEST_F(BootstrapErrPathTest, ConsumeTwiceSecondIsBootstrapInvalidUsed) {
    auto gen = handler_->GenerateToken();
    ASSERT_TRUE(gen.ok()) << gen.status().message();
    const std::string token = gen.value();
    ASSERT_FALSE(token.empty());

    auto first = handler_->Consume(token);
    ASSERT_TRUE(first.ok()) << first.status().message();  // mints the admin user/key

    auto second = handler_->Consume(token);
    ASSERT_FALSE(second.ok());
    EXPECT_TRUE(HasCx(second.status(), "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID"))
        << second.status().message();
    EXPECT_NE(second.status().message().find("reason=used"), std::string::npos)
        << second.status().message();
}

// The AuthStatus mapping for this code is kUnauthenticated (401) -- pin it so a
// recategorization breaks the test (the route layer renders 401).
TEST(BootstrapErrPathStatic, BootstrapInvalidMapsToUnauthenticated) {
    EXPECT_EQ(auth::AuthErrorToStatusCode(AuthErrorCode::kBootstrapTokenInvalid),
              StatusCode::kUnauthenticated);
    EXPECT_STREQ(AuthErrorCodeString(AuthErrorCode::kBootstrapTokenInvalid),
                 "CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID");
}

// ===========================================================================
// CX_ERR_AUTH_ADMIN_REQUIRED -- route layer (system_config PUT).
//
// PUT /api/v1/system/agent_llm_config requires kPermWrite to pass the auth
// middleware, then the handler rejects a non-admin caller (is_admin() false)
// with WriteAuthError(kAdminRequired). We drive it with a key that has Write
// but NOT Admin so the middleware lets us through to the in-handler check.
// ===========================================================================
class AdminRequiredRouteTest : public ::testing::Test {
protected:
    void SetUp() override {
        auth_ = std::make_unique<ApiKeyAuth>();
        writer_key_ = "writer-not-admin-key";
        admin_key_ = "admin-key";
        auth_->LoadKeys({
            MakeKey(writer_key_, "writer", kPermRead | kPermWrite),            // no Admin
            MakeKey(admin_key_, "root", kPermRead | kPermWrite | kPermAdmin),  // admin
        });

        port_ = 19700 + (getpid() % 250);
        server_ = std::make_unique<httplib::Server>();
        // DefaultGlobalConfig: IGlobalConfig has concrete GetAgentLlmConfig /
        // SetAgentLlmConfig; only the 4 GUC getters are pure-virtual.
        RegisterSystemConfigRoutes(*server_, config_, *auth_);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/system/agent_llm_config", Bearer(admin_key_));
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    static ApiKeyConfig MakeKey(const std::string& plaintext, const std::string& tenant,
                                int perms) {
        ApiKeyConfig k;
        k.key_hash = ApiKeyAuth::HashKey(plaintext);
        k.tenant_id = tenant;  // -> AuthContext.user_id == tenant_id (MVP)
        k.permissions = perms;
        return k;
    }

    httplib::Headers Bearer(const std::string& key) {
        return {{"Authorization", "Bearer " + key}};
    }

    // Minimal concrete IGlobalConfig: the PUT handler only needs Get/Set
    // AgentLlmConfig (default impls in the base) -- the 4 GUC getters are
    // unused on this path, so stub them out.
    class StubConfig : public IGlobalConfig {
    public:
        Result<std::string> GetString(const std::string&) const override {
            return Status::InvalidArgument("x");
        }
        Result<bool> GetBool(const std::string&) const override {
            return Status::InvalidArgument("x");
        }
        Result<int> GetInt(const std::string&) const override {
            return Status::InvalidArgument("x");
        }
        Result<float> GetFloat(const std::string&) const override {
            return Status::InvalidArgument("x");
        }
        void OnChange(std::function<void(const std::string&)>) override {}
    };

    StubConfig config_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string writer_key_, admin_key_;
};

// A Write-but-not-Admin caller hits the in-handler admin gate -> 403 +
// CX_ERR_AUTH_ADMIN_REQUIRED envelope.
TEST_F(AdminRequiredRouteTest, NonAdminPutAgentLlmConfigIsAdminRequired) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"provider", "openai"}};
    auto res = cli.Put("/api/v1/system/agent_llm_config", Bearer(writer_key_),
                       body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);  // AdminRequired -> kPermissionDenied -> 403

    auto parsed = json::parse(res->body);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_EQ(parsed["error"]["code"], "CX_ERR_AUTH_ADMIN_REQUIRED");
}

// Control: the admin caller passes the gate (so the 403 above is the admin gate,
// not a blanket rejection). A well-formed admin PUT succeeds (200).
TEST_F(AdminRequiredRouteTest, AdminPutAgentLlmConfigPassesAdminGate) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"provider", "openai"}};
    auto res = cli.Put("/api/v1/system/agent_llm_config", Bearer(admin_key_),
                       body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_NE(res->status, 403)
        << "admin caller must not hit the admin-required gate; body=" << res->body;
}

}  // namespace
}  // namespace cortrix
