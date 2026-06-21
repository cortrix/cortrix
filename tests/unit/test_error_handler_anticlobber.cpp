// 🔒 http_server error_handler anti-clobber (http_server.cpp l.371). The 404
// error handler must fill the generic "resource not found" body ONLY when the
// response body is still EMPTY. A route handler that already wrote a business
// CX_ERR_* body + status 404 (e.g. CX_ERR_F13_SESSION_NOT_FOUND) must survive
// untouched — this is the exact mem-documented "generic 404 ≠ route miss" bug
// class. Driven through a real httplib server with custom routes registered AFTER
// RegisterRoutes() (so the error handler is already installed).
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "httplib.h"

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/config/config.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

class ErrorHandlerAntiClobberTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_anticlobber_" + std::to_string(getpid());
        std::system(("mkdir -p " + tmp_dir_).c_str());

        config_.server.host = "127.0.0.1";
        config_.server.port = 19400 + (getpid() % 400);
        config_.server.thread_count = 2;
        config_.auth.enabled = false;
        config_.ns.data_dir = tmp_dir_;

        ns_mgr_ = std::make_unique<NamespaceManager>(tmp_dir_);
        ns_mgr_->Init();
        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
        server_->RegisterRoutes();  // installs the 404 error handler

        // A route that emits a BUSINESS error body + 404 (handler ran, wrote body).
        server_->server().Get(
            "/api/v1/business-404",
            [](const httplib::Request&, httplib::Response& res) {
                json body;
                body["error"]["code"] = "CX_ERR_F13_SESSION_NOT_FOUND";
                body["error"]["message"] = "session sess_abc not found";
                res.set_content(body.dump(), "application/json");
                res.status = 404;  // 4xx → error handler is invoked by httplib
            });

        // A route that sets 404 but leaves the body EMPTY (handler intentionally
        // does not write) → the generic fill SHOULD apply.
        server_->server().Get(
            "/api/v1/empty-404",
            [](const httplib::Request&, httplib::Response& res) {
                res.status = 404;  // no body written
            });
    }

    void TearDown() override {
        if (server_) server_->Stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::system(("rm -rf " + tmp_dir_).c_str());
    }

    void StartServer() {
        server_thread_ = std::thread([this] { server_->Start(); });
        httplib::Client cli("127.0.0.1", config_.server.port);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/health");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    std::string tmp_dir_;
};

// --- A handler-written business 404 body is NOT clobbered to the generic one ---
TEST_F(ErrorHandlerAntiClobberTest, BusinessBodyNotClobbered) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/business-404");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    // The handler's specific business code must survive — anti-clobber rule.
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F13_SESSION_NOT_FOUND");
    EXPECT_EQ(body["error"]["message"], "session sess_abc not found");
    // It must NOT have been overwritten by the generic NOT_FOUND message.
    EXPECT_NE(body["error"]["message"], "The requested resource was not found");
}

// --- An empty-body 404 IS filled with the generic envelope ---
TEST_F(ErrorHandlerAntiClobberTest, EmptyBodyGetsGenericFill) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/empty-404");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
    EXPECT_EQ(body["error"]["message"], "The requested resource was not found");
    EXPECT_TRUE(body["error"].contains("request_id"));
}

// --- A truly unmatched route also gets the generic fill (regression guard) ---
TEST_F(ErrorHandlerAntiClobberTest, UnmatchedRouteGetsGenericFill) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/no-such-route");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

}  // namespace
}  // namespace cortrix
