#include <gtest/gtest.h>

#include <cstdlib>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/auth/jwt_secret_service.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/server/routes/auth_routes.h"

// FA1 R11 coverage: the P08 §2.11 POST /api/v1/admin/auth/rotate-jwt-secret route.
// The rotation mechanics are unit-tested in test_jwt_secret_rotate.cpp; here we
// cover the HTTP boundary — admin gate + the Agent-friendly RotationResult body.
namespace cortrix {
namespace {

using json = nlohmann::json;

class JwtRotateRouteTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::unsetenv("CORTRIX_JWT_SECRET");
        ASSERT_TRUE(platform_db_.Open(":memory:").ok());
        jwt_ = std::make_unique<auth::JwtSecretService>(platform_db_.db());
        ASSERT_TRUE(jwt_->LoadOrInit().ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "rotate-admin-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "admin-user";
        admin_kc.permissions = kPermRead | kPermAdmin;

        user_key_ = "rotate-readonly-key";
        ApiKeyConfig user_kc;
        user_kc.key_hash = ApiKeyAuth::HashKey(user_key_);
        user_kc.tenant_id = "alice";
        user_kc.permissions = kPermRead;
        auth_->LoadKeys({admin_kc, user_kc});

        server_ = std::make_unique<httplib::Server>();
        RegisterJwtRotateRoute(*server_, *jwt_, *auth_);
        port_ = 19700 + (getpid() % 400);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        jwt_.reset();
        platform_db_.Close();
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers User() { return {{"Authorization", "Bearer " + user_key_}}; }

    auth::PlatformDb platform_db_;
    std::unique_ptr<auth::JwtSecretService> jwt_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
    std::string user_key_;
};

TEST_F(JwtRotateRouteTest, AdminRotateReturnsRotationResult) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/admin/auth/rotate-jwt-secret", Admin(),
                        std::string(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["rotated"], true);
    EXPECT_FALSE(body["new_key_id"].get<std::string>().empty());
    EXPECT_FALSE(body["prev_key_id"].get<std::string>().empty());
    EXPECT_GT(body["prev_expires_at"].get<int64_t>(), 0);
    EXPECT_TRUE(body.contains("warning"));
}

TEST_F(JwtRotateRouteTest, NonAdminIsForbidden) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/admin/auth/rotate-jwt-secret", User(),
                        std::string(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
}

TEST_F(JwtRotateRouteTest, NoAuthIsUnauthenticated) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/admin/auth/rotate-jwt-secret", std::string(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

}  // namespace
}  // namespace cortrix
