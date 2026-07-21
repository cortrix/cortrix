#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <cstdlib>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/common/version.h"
#include "cortrix/deploy/health_routes.h"
#include "cortrix/health/readiness.h"
#include "cortrix/logging/logging.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"

namespace cortrix {
namespace {

class HealthEndpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_health_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        config_.server.host = "127.0.0.1";
        config_.server.port = 0;  // Will use a fixed port
        config_.server.thread_count = 2;
        config_.auth.enabled = false;
        config_.ns.data_dir = tmp_dir_;

        // Find a free port
        port_ = 18080 + (getpid() % 1000);
        config_.server.port = port_;

        ns_mgr_ = std::make_unique<NamespaceManager>(config_.ns.data_dir);
        ns_mgr_->Init();

        auth_.LoadKeys(config_.auth.api_keys);

        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
        server_->RegisterRoutes();

        server_thread_ = std::thread([this] { server_->Start(); });

        // Wait for server to start
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
};

TEST_F(HealthEndpointTest, ReturnsHealthy) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/health");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["status"], "healthy");
    EXPECT_EQ(body["version"], cortrix::kCortrixVersion);  // [P5] version SoT (1.0.0-rc.1)
    EXPECT_TRUE(body.contains("uptime_seconds"));
    EXPECT_EQ(body["components"]["config"], "ok");
    EXPECT_EQ(body["components"]["logging"], "ok");
    EXPECT_EQ(body["components"]["namespace_manager"], "ok");
}

// DEFECT#8: /health reflects the OCR parser provisioning outcome via the
// CORTRIX_PARSER_STATUS env var (set by the deploy entrypoint after provisioning),
// read per-request. Absent → component omitted + "healthy" (back-compat); a failed
// parser stack ("unavailable") degrades overall status so it is not silently green.
TEST_F(HealthEndpointTest, ParserStatusReflectedInComponents) {
    httplib::Client cli("127.0.0.1", port_);

    // Unset: no parser component, status stays healthy.
    ::unsetenv("CORTRIX_PARSER_STATUS");
    {
        auto res = cli.Get("/api/v1/health");
        ASSERT_TRUE(res);
        auto body = nlohmann::json::parse(res->body);
        EXPECT_EQ(body["status"], "healthy");
        EXPECT_FALSE(body["components"].contains("parser"));
    }

    // Failed provisioning → component "unavailable" + overall "degraded".
    ::setenv("CORTRIX_PARSER_STATUS", "unavailable", 1);
    {
        auto res = cli.Get("/api/v1/health");
        ASSERT_TRUE(res);
        auto body = nlohmann::json::parse(res->body);
        EXPECT_EQ(body["components"]["parser"], "unavailable");
        EXPECT_EQ(body["status"], "degraded");
    }

    // Provisioned OK → component "ok", status not degraded.
    ::setenv("CORTRIX_PARSER_STATUS", "ok", 1);
    {
        auto res = cli.Get("/api/v1/health");
        ASSERT_TRUE(res);
        auto body = nlohmann::json::parse(res->body);
        EXPECT_EQ(body["components"]["parser"], "ok");
        EXPECT_EQ(body["status"], "healthy");
    }

    // Lite profile → OCR intentionally off, reported but not degraded.
    ::setenv("CORTRIX_PARSER_STATUS", "disabled", 1);
    {
        auto res = cli.Get("/api/v1/health");
        ASSERT_TRUE(res);
        auto body = nlohmann::json::parse(res->body);
        EXPECT_EQ(body["components"]["parser"], "disabled");
        EXPECT_EQ(body["status"], "healthy");
    }

    ::unsetenv("CORTRIX_PARSER_STATUS");
}

TEST_F(HealthEndpointTest, HasRequestIdHeader) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/health");

    ASSERT_TRUE(res);
    EXPECT_FALSE(res->get_header_value("X-Request-Id").empty());
}

TEST_F(HealthEndpointTest, NotFoundReturnsJsonError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/nonexistent/path");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

TEST_F(HealthEndpointTest, ErrorResponseContainsTimestamp) {
    // Per API_GATEWAY_DESIGN.md section 2.5.1: error responses must include timestamp
    httplib::Client cli("127.0.0.1", port_);

    // 404 handler now uses WriteJsonError() which includes timestamp
    auto res = cli.Get("/api/v1/nonexistent");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
    EXPECT_TRUE(body["error"].contains("timestamp"));
    // Verify timestamp has ISO 8601 format (starts with date)
    std::string ts = body["error"]["timestamp"];
    EXPECT_GE(ts.size(), 20u);  // e.g. "2026-02-16T00:00:00.000Z"
    EXPECT_EQ(ts[4], '-');
    EXPECT_EQ(ts[7], '-');
    EXPECT_EQ(ts[10], 'T');
}

TEST_F(HealthEndpointTest, UptimeIncreases) {
    httplib::Client cli("127.0.0.1", port_);

    auto res1 = cli.Get("/api/v1/health");
    ASSERT_TRUE(res1);
    auto body1 = nlohmann::json::parse(res1->body);
    int64_t uptime1 = body1["uptime_seconds"];

    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto res2 = cli.Get("/api/v1/health");
    ASSERT_TRUE(res2);
    auto body2 = nlohmann::json::parse(res2->body);
    int64_t uptime2 = body2["uptime_seconds"];

    EXPECT_GE(uptime2, uptime1);
}

// [P5] GET /api/v1/system/version — public version info via the deploy health
// routes (issue ⑭). Registers RegisterHealthRoutes on a bare server and asserts
// the {version} body carries the single-SoT version string.
TEST(SystemVersionEndpointTest, ReturnsVersion) {
    httplib::Server svr;
    cortrix::health::ReadinessRegistry registry;
    cortrix::deploy::RegisterHealthRoutes(
        svr, registry, [] { return false; }, cortrix::kCortrixVersion);

    const int port = 18090 + (getpid() % 900);
    std::thread t([&] { svr.listen("127.0.0.1", port); });
    httplib::Client cli("127.0.0.1", port);
    for (int i = 0; i < 50 && !cli.Get("/api/v1/system/version"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto res = cli.Get("/api/v1/system/version");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["version"], cortrix::kCortrixVersion);
    EXPECT_EQ(std::string(cortrix::kCortrixVersion), "1.0.0-rc.1");

    svr.stop();
    if (t.joinable()) t.join();
}

}  // namespace
}  // namespace cortrix
