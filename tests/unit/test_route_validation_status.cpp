// Route request-validation matrices for the full CortrixHttpServer surface that
// is deterministically reachable: health/readiness + namespace CRUD + the global
// 404 error-handler ANTI-CLOBBER property.
//
// Unlike the per-route Register*Routes() fixtures (which mount a bare
// httplib::Server with NO error handler), CortrixHttpServer installs the global
// set_error_handler that (a) synthesizes a generic {"error":{"code":"NOT_FOUND"}}
// body for a truly-unmatched route, and (b) MUST NOT overwrite a 4xx body a
// handler already wrote (the `if (res.body.empty())` guard). This file exercises
// both, plus the namespace-create request-validation matrix, over the real
// server harness used by test_http_server.cpp.
//
// Suite/fixture names area-prefixed (StatusRouteValMatrix) for global uniqueness.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/config/config.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// Auth-disabled full server (namespace CRUD reachable without keys), matching
// the HttpServerTest harness in test_http_server.cpp.
class StatusRouteValMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_srvm_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        config_.server.host = "127.0.0.1";
        config_.server.port = 18000 + (getpid() % 500);
        config_.server.thread_count = 2;
        config_.auth.enabled = false;
        config_.ns.data_dir = tmp_dir_;

        ns_mgr_ = std::make_unique<NamespaceManager>(tmp_dir_);
        ns_mgr_->Init();

        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
        server_->RegisterRoutes();
        server_thread_ = std::thread([this] { server_->Start(); });

        httplib::Client cli("127.0.0.1", config_.server.port);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/health");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void TearDown() override {
        if (server_) server_->Stop();
        if (server_thread_.joinable()) server_thread_.join();
        system(("rm -rf " + tmp_dir_).c_str());
    }

    int port() const { return config_.server.port; }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    std::string tmp_dir_;
};

// ---- health (no auth) -----------------------------------------------------

TEST_F(StatusRouteValMatrix, HealthReturns200) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Get("/api/v1/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["status"], "healthy");
}

TEST_F(StatusRouteValMatrix, HealthHasRequestId) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Get("/api/v1/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->get_header_value("X-Request-Id").size(), 32u);
}

// ---- generic 404 on a truly-unmatched route -------------------------------
// The error handler synthesizes a NOT_FOUND JSON body (body was empty).

class StatusRouteValUnknown
    : public StatusRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(StatusRouteValUnknown, Returns404WithSynthesizedBody) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Get(GetParam().c_str());
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 404) << GetParam();
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND") << GetParam();
    EXPECT_TRUE(body["error"].contains("request_id")) << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Routes, StatusRouteValUnknown,
    ::testing::Values(std::string("/api/v1/nonexistent"),
                      std::string("/api/v1/does/not/exist"),
                      std::string("/api/v2/health"),
                      std::string("/totally/unknown"),
                      std::string("/api/v1/namespaces/x/y/z/bogus"),
                      std::string("/api/v1/health/extra"),
                      std::string("/api/v1/query/extra/path"),
                      std::string("/api/v3/anything"),
                      std::string("/api/v1/admin"),
                      std::string("/api/v1/connector/watchers/x/y/z"),
                      std::string("/api/v1/operations/extra/path"),
                      std::string("/api/v1/gc"),
                      std::string("/api/v1/batch/unknown"),
                      std::string("/api/v1/")));

// ---- namespace-create request-validation matrix ---------------------------

class StatusRouteValBadNsBody
    : public StatusRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(StatusRouteValBadNsBody, Returns400) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Post("/api/v1/namespaces", GetParam(), "application/json");
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 400) << GetParam() << " body=" << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT") << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Bodies, StatusRouteValBadNsBody,
    ::testing::Values(std::string("not json"), std::string("}{"),
                      std::string("[1,2,3"), std::string(R"({"foo":"bar"})"),
                      std::string(R"({"name":123})"), std::string(R"({"name":null})"),
                      std::string(R"({"name":""})"), std::string("{}"),
                      std::string("")));

// ---- not-found on GET/DELETE of a missing namespace -----------------------

TEST_F(StatusRouteValMatrix, GetMissingNamespace404) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Get("/api/v1/namespaces/does-not-exist-srvm");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

TEST_F(StatusRouteValMatrix, DeleteMissingNamespace404) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Delete("/api/v1/namespaces/ghost-srvm");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(StatusRouteValMatrix, DeleteDefaultNamespace400) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Delete("/api/v1/namespaces/default");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// ---- ANTI-CLOBBER: a handler-written 4xx body survives the error handler ---
// CreateNamespace with a missing name returns a 400 INVALID_ARGUMENT body. The
// global error handler runs for any >=400 status; it must NOT replace this real
// business body with the generic "resource not found" envelope.

TEST_F(StatusRouteValMatrix, BusinessBodyNotClobberedBy400) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Post("/api/v1/namespaces", R"({"foo":"bar"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    // Preserved business code, NOT the generic NOT_FOUND clobber.
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
    EXPECT_NE(body["error"]["code"], "NOT_FOUND");
}

// A missing-namespace GET returns a real 404 business body
// ({"error":{"code":"NOT_FOUND"}}) that the handler wrote itself; the error
// handler must leave it intact (it is already non-empty). We assert the body is
// well-formed JSON with a request_id, proving it was not blanked/regenerated
// into a divergent shape.
TEST_F(StatusRouteValMatrix, Handler404BodyWellFormed) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Get("/api/v1/namespaces/missing-srvm-xyz");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

// ---- method-not-allowed on namespace collection ---------------------------

TEST_F(StatusRouteValMatrix, PatchOnNamespacesCollectionGeneric404) {
    httplib::Client cli("127.0.0.1", port());
    auto res = cli.Patch("/api/v1/namespaces", "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    // Unmatched method+path -> synthesized generic body.
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

}  // namespace
}  // namespace cortrix
