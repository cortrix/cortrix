// http_server.cpp "pure function" coverage. The targets — ResolveError /
// ExtractCxToken / Sdk3Map / StatusCodeToCategory / StatusCodeRetryable /
// YamlScalarToJson / IsSafeOpenApiAssetPath / the SPA-vs-404 error_handler branch
// — live in an ANONYMOUS namespace in http_server.cpp, so they are not directly
// linkable. They are exercised through their public entry points:
//
//   * WriteJsonError(res, Status, request_id)  → ResolveError → ExtractCxToken +
//     Sdk3Map hit/miss + StatusCodeToCategory/Retryable fallback (NO socket).
//   * the in-process OpenAPI asset routes      → IsSafeOpenApiAssetPath traversal
//     rejects + YamlScalarToJson (via /openapi.json YAML→JSON) (loopback server).
//   * the registered error_handler             → SPA-index fallback vs generic 404.
//
// Suite/fixture names are module-prefixed (HttpServerPureFn*) for global gtest
// uniqueness. Deterministic: no live network beyond 127.0.0.1 loopback, no LLM.
// Temp asset dir is unique per process (getpid + atomic counter) per the parallel
// -j2 process-isolation rule.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/common/status.h"
#include "cortrix/config/config.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// ===========================================================================
// ResolveError / Sdk3Map / StatusCodeToCategory — driven via WriteJsonError.
// No socket: WriteJsonError writes straight into an httplib::Response.
// ===========================================================================

// Helper: run WriteJsonError on a Status and return the parsed error body.
json ErrBody(const Status& s) {
    httplib::Response res;
    WriteJsonError(res, s, "rid-pure");
    return json::parse(res.body)["error"];
}

// --- Rich CX token IN the sec.3 SoT map: code preserved + map's category/retry --
TEST(HttpServerPureFnResolve, Sec3MapHit_TimeoutRetryable) {
    // CX_ERR_AGENT_LLM_TIMEOUT → {kTimeout, retryable=true} per Sdk3Map.
    auto e = ErrBody(Status::Internal("CX_ERR_AGENT_LLM_TIMEOUT: model stalled"));
    EXPECT_EQ(e["code"], "CX_ERR_AGENT_LLM_TIMEOUT");
    EXPECT_EQ(e["category"], "timeout");
    EXPECT_EQ(e["retryable"], true);
}

TEST(HttpServerPureFnResolve, Sec3MapHit_QuotaRetryable) {
    auto e = ErrBody(Status::PermissionDenied("CX_ERR_RATE_LIMITED: slow down"));
    EXPECT_EQ(e["code"], "CX_ERR_RATE_LIMITED");
    EXPECT_EQ(e["category"], "quota");
    EXPECT_EQ(e["retryable"], true);
}

TEST(HttpServerPureFnResolve, Sec3MapHit_AuthNotRetryable) {
    auto e = ErrBody(Status::PermissionDenied("CX_ERR_NS_UNAUTHORIZED"));
    EXPECT_EQ(e["code"], "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_EQ(e["category"], "auth");
    EXPECT_EQ(e["retryable"], false);
}

// --- Rich token NOT in the sec.3 map: code kept, category/retry from StatusCode --
TEST(HttpServerPureFnResolve, RichTokenMiss_FallsBackToStatusCode) {
    // CX_ERR_WIDGET_X is not in Sdk3Map; carried on a NotFound Status (404) →
    // StatusCodeToCategory(kNotFound)=permanent, StatusCodeRetryable(404)=false.
    auto e = ErrBody(Status::NotFound("CX_ERR_WIDGET_X: gone"));
    EXPECT_EQ(e["code"], "CX_ERR_WIDGET_X");
    EXPECT_EQ(e["category"], "permanent");
    EXPECT_EQ(e["retryable"], false);
}

TEST(HttpServerPureFnResolve, RichTokenMiss_InternalIsTransientRetryable) {
    // Unmapped token on a 500 → StatusCodeToCategory(kInternal)=transient, retry=true.
    auto e = ErrBody(Status::Internal("CX_ERR_WIDGET_Y: boom"));
    EXPECT_EQ(e["code"], "CX_ERR_WIDGET_Y");
    EXPECT_EQ(e["category"], "transient");
    EXPECT_EQ(e["retryable"], true);
}

// --- No CX token at all: generic error_code_string + StatusCode category --------
TEST(HttpServerPureFnResolve, NoToken_UsesStatusCodeString) {
    // Plain message, no "CX_ERR_" prefix → ExtractCxToken returns "" → the generic
    // error_code_string() is used and category derives from the StatusCode.
    auto e = ErrBody(Status::InvalidArgument("bad input"));
    EXPECT_EQ(e["code"], "INVALID_ARGUMENT");   // error_code_string for 400
    EXPECT_EQ(e["category"], "permanent");      // kInvalidArgument → permanent
    EXPECT_EQ(e["retryable"], false);
}

TEST(HttpServerPureFnResolve, NoToken_UnauthenticatedIsAuth) {
    auto e = ErrBody(Status::Unauthenticated("Missing Authorization header"));
    EXPECT_EQ(e["category"], "auth");           // kUnauthenticated → auth
    EXPECT_EQ(e["retryable"], false);
}

TEST(HttpServerPureFnResolve, NoToken_UnavailableIsTransientRetryable) {
    auto e = ErrBody(Status::Unavailable("down for maintenance"));
    EXPECT_EQ(e["category"], "transient");      // kUnavailable → transient
    EXPECT_EQ(e["retryable"], true);            // StatusCodeRetryable(503)=true
}

// ===========================================================================
// In-process server: drives the OpenAPI asset routes (IsSafeOpenApiAssetPath +
// YamlScalarToJson) and the error_handler SPA-vs-404 branch.
// ===========================================================================

std::string UniquePureDir() {
    static std::atomic<unsigned> ctr{0};
    auto p = std::filesystem::temp_directory_path() /
             ("cortrix_purefn_" + std::to_string(::getpid()) + "_" +
              std::to_string(ctr.fetch_add(1)));
    std::filesystem::create_directories(p);
    return p.string();
}

class HttpServerPureFnServer : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = UniquePureDir();
        data_dir_ = root_ + "/data";
        std::filesystem::create_directories(data_dir_);
        // Lay out the OpenAPI asset tree that ReadOpenApiAsset / OpenApiJson serve.
        std::filesystem::create_directories(root_ + "/api/paths");
        // A valid YAML doc whose scalars hit every YamlScalarToJson arm.
        std::filesystem::create_directories(root_ + "/api");
        {
            std::ofstream y(root_ + "/api/openapi.yaml");
            y << "openapi: 3.0.0\n"
                 "flag_true: true\n"
                 "flag_false: false\n"
                 "nothing: null\n"
                 "count: 42\n"
                 "ratio: 3.5\n"
                 "big: 99999999999999999999999999\n";  // overflow → stays string
        }
        {
            std::ofstream a(root_ + "/api/paths/widget.yaml");
            a << "get:\n  summary: widget\n";
        }
        setenv("CORTRIX_OPENAPI_ROOT", root_.c_str(), 1);

        config_.server.host = "127.0.0.1";
        config_.server.port = 19700 + (getpid() % 250);
        config_.server.thread_count = 2;
        config_.auth.enabled = false;
        config_.ns.data_dir = data_dir_;

        ns_mgr_ = std::make_unique<NamespaceManager>(data_dir_);
        ns_mgr_->Init();
        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
    }

    void TearDown() override {
        if (server_) server_->Stop();
        if (server_thread_.joinable()) server_thread_.join();
        unsetenv("CORTRIX_OPENAPI_ROOT");
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    void StartServer() {
        server_->RegisterRoutes();
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
    std::string root_;
    std::string data_dir_;
};

// IsSafeOpenApiAssetPath: a legit relative path is served.
TEST_F(HttpServerPureFnServer, OpenApiAssetSafePathServed) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/paths/widget.yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("summary"), std::string::npos);
}

// IsSafeOpenApiAssetPath: a ".." traversal segment is rejected. The regex route
// /(paths|components)/(.+\.ya?ml) still matches "evil/../x.yaml" but the path
// guard rejects it → NotFound (the asset is not read).
TEST_F(HttpServerPureFnServer, OpenApiAssetTraversalRejected) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/paths/..%2f..%2fwidget.yaml");
    ASSERT_TRUE(res);
    // Either the route matched and the guard rejected (404 JSON), or the raw "/.."
    // never matched — both are a 404 with no asset body. Assert it is NOT served.
    EXPECT_EQ(res->status, 404);
}

// YamlScalarToJson arms via /openapi.json (YAML → JSON coercion of every scalar).
TEST_F(HttpServerPureFnServer, OpenApiJsonCoercesYamlScalars) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/openapi.json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_TRUE(body["flag_true"].is_boolean());
    EXPECT_EQ(body["flag_true"], true);
    EXPECT_EQ(body["flag_false"], false);
    EXPECT_TRUE(body["nothing"].is_null());
    EXPECT_TRUE(body["count"].is_number_integer());
    EXPECT_EQ(body["count"], 42);
    EXPECT_TRUE(body["ratio"].is_number_float());
    EXPECT_FLOAT_EQ(body["ratio"].get<double>(), 3.5);
    // Overflowing integer literal keeps its original string form (stoll throws →
    // caught → returns the string).
    EXPECT_TRUE(body["big"].is_string());
}

// error_handler: with NO web UI configured, an unmatched non-/api GET is a generic
// JSON 404 (the SPA branch is skipped because web_ui_index_ is empty).
TEST_F(HttpServerPureFnServer, ErrorHandlerGeneric404WhenNoWebUi) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/some/spa/deep/link");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

// error_handler: with the web UI enabled, the SAME unmatched non-/api GET serves
// the SPA index.html (200) — the SPA-vs-404 branch's other arm.
TEST_F(HttpServerPureFnServer, ErrorHandlerServesSpaIndexWhenWebUiEnabled) {
    std::string ui_dir = root_ + "/webui";
    std::filesystem::create_directories(ui_dir);
    { std::ofstream idx(ui_dir + "/index.html"); idx << "<!doctype html><title>SPA</title>"; }
    server_->EnableWebUi(ui_dir);
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/dashboard/deep/link");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("SPA"), std::string::npos);
    // An unmatched /api GET still 404s (the SPA fallback is non-/api only).
    auto api = cli.Get("/api/v1/does-not-exist");
    ASSERT_TRUE(api);
    EXPECT_EQ(api->status, 404);
}

}  // namespace
}  // namespace cortrix
