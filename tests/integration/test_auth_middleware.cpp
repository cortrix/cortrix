#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/logging/logging.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"

namespace cortrix {
namespace {

class AuthMiddlewareIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_auth_integ_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        test_key_ = "my-secret-api-key";
        test_hash_ = ApiKeyAuth::HashKey(test_key_);

        config_.server.host = "127.0.0.1";
        config_.server.thread_count = 2;
        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;

        ApiKeyConfig kc;
        kc.key_hash = test_hash_;
        kc.tenant_id = "test-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        kc.expires_at = 0;
        config_.auth.api_keys.push_back(kc);

        // Read-only key
        read_key_ = "read-only-key";
        std::string read_hash = ApiKeyAuth::HashKey(read_key_);
        ApiKeyConfig rkc;
        rkc.key_hash = read_hash;
        rkc.tenant_id = "read-tenant";
        rkc.permissions = kPermRead;
        rkc.expires_at = 0;
        config_.auth.api_keys.push_back(rkc);

        port_ = 18180 + (getpid() % 1000);
        config_.server.port = port_;

        ns_mgr_ = std::make_unique<NamespaceManager>(config_.ns.data_dir);
        ns_mgr_->Init();

        auth_.LoadKeys(config_.auth.api_keys);

        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
        server_->RegisterRoutes();

        server_thread_ = std::thread([this] { server_->Start(); });

        // Wait for server
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/health");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void TearDown() override {
        server_->Stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        system(("rm -rf " + tmp_dir_).c_str());
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    std::string tmp_dir_;
    int port_;
    std::string test_key_;
    std::string test_hash_;
    std::string read_key_;
};

TEST_F(AuthMiddlewareIntegrationTest, NoHeaderReturns401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/namespaces");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "UNAUTHORIZED");
    EXPECT_EQ(body["error"]["message"], "Missing Authorization header or X-API-Key");
}

TEST_F(AuthMiddlewareIntegrationTest, InvalidKeyReturns401) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {{"Authorization", "Bearer wrong-key"}};
    auto res = cli.Get("/api/v1/namespaces", headers);

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_API_KEY");
}

TEST_F(AuthMiddlewareIntegrationTest, InvalidFormatReturns401) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {{"Authorization", "Basic abc123"}};
    auto res = cli.Get("/api/v1/namespaces", headers);

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "UNAUTHORIZED");
}

TEST_F(AuthMiddlewareIntegrationTest, ValidKeyReturns200) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {{"Authorization", "Bearer " + test_key_}};
    auto res = cli.Get("/api/v1/namespaces", headers);

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(AuthMiddlewareIntegrationTest, InsufficientPermReturns403) {
    httplib::Client cli("127.0.0.1", port_);
    // Use read-only key to try POST (requires ADMIN)
    httplib::Headers headers = {{"Authorization", "Bearer " + read_key_}};
    auto res = cli.Post("/api/v1/namespaces", headers, R"({"name":"test-ns"})", "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "FORBIDDEN");
}

TEST_F(AuthMiddlewareIntegrationTest, HealthEndpointNoAuthRequired) {
    httplib::Client cli("127.0.0.1", port_);
    // /health should work without auth even when auth.enabled=true
    auto res = cli.Get("/api/v1/health");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

}  // namespace
}  // namespace cortrix
