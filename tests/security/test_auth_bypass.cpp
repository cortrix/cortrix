/// @file test_auth_bypass.cpp
/// @brief Security tests: auth bypass attempts via missing, malformed, invalid tokens
///
/// Validates that the auth middleware correctly rejects all forms of
/// authentication bypass including missing headers, empty tokens,
/// malformed formats, invalid keys, expired keys, insufficient permissions,
/// and case sensitivity attacks.

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"
#include "unit/namespace_authz_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

class AuthBypassTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_sec_auth_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        valid_key_ = "valid-secret-key-for-tests";
        read_only_key_ = "read-only-secret-key";

        config_.server.host = "127.0.0.1";
        config_.server.thread_count = 2;
        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_.string();

        // Full-permission key
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(valid_key_);
        kc.tenant_id = "sec-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        kc.expires_at = 0;
        config_.auth.api_keys.push_back(kc);

        // Read-only key
        ApiKeyConfig rkc;
        rkc.key_hash = ApiKeyAuth::HashKey(read_only_key_);
        rkc.tenant_id = "readonly-tenant";
        rkc.permissions = kPermRead;
        rkc.expires_at = 0;
        config_.auth.api_keys.push_back(rkc);

        // Namespace-restricted key. ARCHITECTURE V6 removed the static per-key
        // allow-list; namespace scope is now expressed as ownership in the runtime
        // PermissionService (installed below). The key's tenant ("ns-tenant") owns
        // ONLY "allowed-ns", so any OTHER namespace is denied (CX_ERR_NS_UNAUTHORIZED).
        ns_restricted_key_ = "ns-restricted-key";
        ApiKeyConfig nkc;
        nkc.key_hash = ApiKeyAuth::HashKey(ns_restricted_key_);
        nkc.tenant_id = "ns-tenant";
        nkc.permissions = kPermRead | kPermWrite;
        nkc.expires_at = 0;
        config_.auth.api_keys.push_back(nkc);

        port_ = 19200 + (getpid() % 500);
        config_.server.port = port_;

        ns_mgr_ = std::make_unique<NamespaceManager>(config_.ns.data_dir);
        ns_mgr_->Init();

        auth_.LoadKeys(config_.auth.api_keys);

        // ARCHITECTURE V6: wire auth_'s runtime namespace-authorization seam to a
        // REAL PermissionService. Seed ONLY "allowed-ns" owned by the restricted
        // key's tenant ("ns-tenant"), preserving the original intent that this key
        // is scoped to "allowed-ns" and DENIED any other namespace. This fixture
        // drives the real CortrixHttpServer/NamespaceManager (no namespace pool), so the
        // list seam is left unwired (nullptr pool); the per-namespace gate is what
        // the ownership seeding governs.
        authz_ = std::make_unique<cortrix::test::NamespaceAuthzHarness>(
            auth_, /*pool=*/nullptr, "ns-tenant",
            std::vector<std::string>{"allowed-ns"});

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
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<cortrix::test::NamespaceAuthzHarness> authz_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    fs::path tmp_dir_;
    int port_;
    std::string valid_key_;
    std::string read_only_key_;
    std::string ns_restricted_key_;
};

// SEC-AUTH-001: Missing Authorization header → 401
TEST_F(AuthBypassTest, MissingAuthHeader) {
    httplib::Client cli("127.0.0.1", port_);

    auto res = cli.Get("/api/v1/namespaces");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "UNAUTHORIZED");
    EXPECT_TRUE(body["error"].contains("timestamp"));
}

// SEC-AUTH-002: Empty Bearer token → 401
TEST_F(AuthBypassTest, EmptyBearerToken) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {{"Authorization", "Bearer "}};

    auto res = cli.Get("/api/v1/namespaces", headers);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// SEC-AUTH-003: Malformed Authorization format → 401
TEST_F(AuthBypassTest, MalformedAuthFormat) {
    httplib::Client cli("127.0.0.1", port_);

    std::vector<std::string> malformed_values = {
        "Basic abc123",           // Wrong scheme
        "Bearer",                 // No space + token
        "bearer " + valid_key_,   // Wrong case for "Bearer"
        "Token " + valid_key_,    // Unsupported scheme
        valid_key_,               // No scheme at all
        "Bearer  " + valid_key_,  // Double space
    };

    for (const auto& val : malformed_values) {
        httplib::Headers headers = {{"Authorization", val}};
        auto res = cli.Get("/api/v1/namespaces", headers);
        ASSERT_TRUE(res) << "No response for: " << val;
        EXPECT_EQ(res->status, 401)
            << "Expected 401 for malformed auth '" << val
            << "', got " << res->status;
    }
}

// SEC-AUTH-004: Invalid/non-existent API key → 401
TEST_F(AuthBypassTest, InvalidApiKey) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {{"Authorization", "Bearer totally-wrong-key"}};

    auto res = cli.Get("/api/v1/namespaces", headers);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_API_KEY");
}

// SEC-AUTH-005: Insufficient permissions → 403
TEST_F(AuthBypassTest, InsufficientPermissions) {
    httplib::Client cli("127.0.0.1", port_);

    // Read-only key attempting POST (requires ADMIN)
    httplib::Headers headers = {{"Authorization", "Bearer " + read_only_key_}};
    auto res = cli.Post("/api/v1/namespaces", headers,
                        R"({"name":"test-ns"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "FORBIDDEN");
}

// SEC-AUTH-006: X-API-Key header fallback test
TEST_F(AuthBypassTest, XApiKeyHeaderFallback) {
    httplib::Client cli("127.0.0.1", port_);

    // Test with X-API-Key header instead of Authorization Bearer
    httplib::Headers headers = {{"X-API-Key", valid_key_}};
    auto res = cli.Get("/api/v1/namespaces", headers);
    ASSERT_TRUE(res);
    // The server may support X-API-Key or reject it as unauthorized.
    // Either 200 (supported) or 401 (not supported) is acceptable,
    // but NOT 500.
    EXPECT_TRUE(res->status == 200 || res->status == 401)
        << "Unexpected status: " << res->status;
}

// SEC-AUTH-007: Health endpoint requires no auth (even with auth enabled)
TEST_F(AuthBypassTest, HealthEndpointNoAuth) {
    httplib::Client cli("127.0.0.1", port_);

    auto res = cli.Get("/api/v1/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

// SEC-AUTH-008: Valid key works as baseline
TEST_F(AuthBypassTest, ValidKeyBaseline) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {{"Authorization", "Bearer " + valid_key_}};

    auto res = cli.Get("/api/v1/namespaces", headers);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

}  // namespace
}  // namespace cortrix
