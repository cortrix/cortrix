// http_server.cpp lifecycle + route-body gap coverage. Complements
// test_http_server.cpp (HttpServerTest / HttpServerAuthEnabledTest) and
// test_http_server_purefns.cpp (HttpServerPureFn*). Suite/fixture names are
// module-prefixed (HttpServerLifecycle) for global gtest uniqueness.
//
// Targets http_server.cpp lines the existing suites do not reach:
//   * the global set_exception_handler envelope -- CX_ERR_INTERNAL_ERROR / 500 /
//     X-Request-Id echo-vs-mint (lines 387-411). Driven by registering a route that
//     throws onto the SAME server whose RegisterRoutes() installed the handler.
//   * the /docs swagger HTML route (415-441).
//   * the OpenAPI not-found arms when no spec files exist -- /openapi.bundled.yaml,
//     /openapi.json, /(paths|components)/... (461-465, 478-485).
//   * GET /api/v1/system/features (628-639).
//   * GET /api/v1/system/namespaces/:ns/stats -- the authz-denied arm (596-599) and
//     the success arm (602-616), both under auth.
//   * Start() bind-failure (523-525) -- a second listen on a held port.
//   * SetNamespacePool (539-541).
//
// Deterministic: 127.0.0.1 loopback only, no LLM, no real model. Temp dirs are
// per-process (getpid + atomic counter) per the parallel -j2 isolation rule.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/config/config.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

std::string UniqueLifeDir() {
    static std::atomic<unsigned> ctr{0};
    auto p = std::filesystem::temp_directory_path() /
             ("cortrix_http_life_" + std::to_string(::getpid()) + "_" +
              std::to_string(ctr.fetch_add(1)));
    std::filesystem::create_directories(p);
    return p.string();
}

// Base fixture: a CortrixHttpServer over a real NamespaceManager + temp data dir.
// `auth_enabled_` / `admin_key_` let subtests pick the auth-on or auth-off wiring.
class HttpServerLifecycle : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = UniqueLifeDir();
        data_dir_ = root_ + "/data";
        std::filesystem::create_directories(data_dir_);

        // No OpenAPI spec tree under this root -> the openapi routes hit their
        // not-found arms. Point the loader at our empty root.
        setenv("CORTRIX_OPENAPI_ROOT", root_.c_str(), 1);

        static std::atomic<unsigned> port_ctr{0};
        config_.server.host = "127.0.0.1";
        config_.server.port = 19850 + ((getpid() + port_ctr.fetch_add(1)) % 140);
        config_.server.thread_count = 2;
        config_.auth.enabled = auth_enabled_;
        config_.ns.data_dir = data_dir_;

        if (auth_enabled_) {
            ApiKeyConfig kc;
            kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
            kc.tenant_id = "test-tenant";
            kc.permissions = kPermRead | kPermWrite | kPermAdmin;
            kc.expires_at = 0;
            config_.auth.api_keys.push_back(kc);
            auth_.LoadKeys(config_.auth.api_keys);
        }

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

    // Register routes + (optionally) extra routes, then start + wait for readiness.
    void StartServer() {
        server_->RegisterRoutes();
        if (extra_routes_) extra_routes_(server_->server());
        server_thread_ = std::thread([this] { server_->Start(); });
        httplib::Client cli("127.0.0.1", config_.server.port);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/health");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }

    bool auth_enabled_ = false;
    std::string admin_key_ = "life-admin-key";
    std::function<void(httplib::Server&)> extra_routes_;

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    std::string root_;
    std::string data_dir_;
};

// ---------------------------------------------------------------------------
// Global exception handler (lines 387-411).
// ---------------------------------------------------------------------------

// A route that throws std::runtime_error escapes to the server's exception handler,
// which re-emits the GEN-Agent envelope: CX_ERR_INTERNAL_ERROR, 500, retryable=false,
// category=permanent, message including what(). With NO inbound X-Request-Id the
// handler MINTS a fresh 32-hex id (line 404).
TEST_F(HttpServerLifecycle, ExceptionHandlerEnvelopeAndMintedRequestId) {
    extra_routes_ = [](httplib::Server& svr) {
        svr.Get("/boom", [](const httplib::Request&, httplib::Response&) {
            throw std::runtime_error("kaboom");
        });
    };
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/boom");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 500) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_INTERNAL_ERROR");
    EXPECT_EQ(body["error"]["category"], "permanent");
    EXPECT_EQ(body["error"]["retryable"], false);
    EXPECT_NE(body["error"]["message"].get<std::string>().find("kaboom"), std::string::npos);
    // A fresh id was minted and echoed.
    std::string rid = res->get_header_value("X-Request-Id");
    EXPECT_EQ(rid.size(), 32u);
    EXPECT_EQ(body["error"]["request_id"], rid);
}

// When the client SUPPLIES an X-Request-Id, the exception handler echoes that exact
// correlation id rather than minting one (line 403 true arm).
TEST_F(HttpServerLifecycle, ExceptionHandlerEchoesInboundRequestId) {
    extra_routes_ = [](httplib::Server& svr) {
        svr.Get("/boom2", [](const httplib::Request&, httplib::Response&) {
            throw std::runtime_error("again");
        });
    };
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/boom2", {{"X-Request-Id", "client-supplied-rid"}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 500) << res->body;
    EXPECT_EQ(res->get_header_value("X-Request-Id"), "client-supplied-rid");
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["request_id"], "client-supplied-rid");
}

// A non-std::exception throw routes through the catch(...) arm (line 398-399).
TEST_F(HttpServerLifecycle, ExceptionHandlerUnknownThrow) {
    extra_routes_ = [](httplib::Server& svr) {
        svr.Get("/boom3", [](const httplib::Request&, httplib::Response&) {
            throw 42;  // non-std::exception
        });
    };
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/boom3");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 500) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_INTERNAL_ERROR");
    EXPECT_NE(body["error"]["message"].get<std::string>().find("unknown exception"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// /docs swagger HTML (lines 415-441).
// ---------------------------------------------------------------------------

TEST_F(HttpServerLifecycle, DocsServesSwaggerHtml) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/docs");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->get_header_value("Content-Type"), "text/html");
    EXPECT_NE(res->body.find("swagger-ui"), std::string::npos);
}

// ---------------------------------------------------------------------------
// OpenAPI not-found arms (no spec files under CORTRIX_OPENAPI_ROOT).
// ---------------------------------------------------------------------------

TEST_F(HttpServerLifecycle, OpenApiYamlNotFound404) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/openapi.bundled.yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

TEST_F(HttpServerLifecycle, OpenApiJsonNotFound404) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    // Neither build/openapi.json nor api/openapi.yaml exist -> the final NotFound arm.
    auto res = cli.Get("/openapi.json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "NOT_FOUND");
}

TEST_F(HttpServerLifecycle, OpenApiAssetNotFound404) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    // Safe relative path, but no file on disk -> ReadOpenApiAsset miss -> NotFound.
    auto res = cli.Get("/paths/missing.yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "NOT_FOUND");
}

// ---------------------------------------------------------------------------
// GET /api/v1/system/features (lines 628-639).
// ---------------------------------------------------------------------------

TEST_F(HttpServerLifecycle, SystemFeaturesReturnsEditionAndFlags) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/system/features");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["edition"], "ce");
    EXPECT_EQ(body["features"]["cdc"], false);
    EXPECT_EQ(body["features"]["text_to_sql"], false);
    EXPECT_EQ(body["features"]["fuse"], false);
    EXPECT_EQ(body["features"]["s3_proxy"], false);
}

// ---------------------------------------------------------------------------
// system/namespaces/:ns/stats -- no-auth success arm (lines 602-616).
// ---------------------------------------------------------------------------

TEST_F(HttpServerLifecycle, NamespaceStatsNoAuthSuccess) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    // Create a namespace, then read its stats. auth disabled -> NoAuth(write_ns_stats).
    auto created = cli.Post("/api/v1/namespaces", R"({"name":"stats-ns"})", "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;

    auto res = cli.Get("/api/v1/system/namespaces/stats-ns/stats");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["namespace"], "stats-ns");
    EXPECT_TRUE(body.contains("document_count"));
    EXPECT_TRUE(body.contains("chunk_count"));
    EXPECT_TRUE(body.contains("storage_bytes"));
    EXPECT_TRUE(body.contains("last_query_at"));
}

// stats on a missing namespace flows BuildNamespaceJson's not-found Status into
// WriteJsonError (lines 603-607).
TEST_F(HttpServerLifecycle, NamespaceStatsUnknownNamespaceError) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/system/namespaces/no-such-ns/stats");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400) << res->body;
}

// ---------------------------------------------------------------------------
// Start() bind-failure (lines 523-525) -- a second server on the held port.
// ---------------------------------------------------------------------------

TEST_F(HttpServerLifecycle, StartBindFailureReturnsInternal) {
    // Force a deterministic, fast bind failure: a port clash is NOT reliable on
    // this platform (httplib sets SO_REUSEADDR, so a second listener on the same
    // 127.0.0.1 port can succeed and block in accept() forever). Instead bind to
    // an address that is not assigned to any local interface (RFC 5737 TEST-NET-1
    // 192.0.2.1): bind() returns EADDRNOTAVAIL, svr_.listen() returns false, and
    // Start() takes the failure arm synchronously (lines 523-525) with no thread.
    CortrixConfig cfg2 = config_;
    cfg2.server.host = "192.0.2.1";
    NamespaceManager ns2(data_dir_ + "2");
    std::filesystem::create_directories(data_dir_ + "2");
    ns2.Init();
    ApiKeyAuth auth2;
    CortrixHttpServer server2(cfg2, auth2, ns2);
    server2.RegisterRoutes();
    Status s = server2.Start();  // returns synchronously on bind failure
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("Failed to bind"), std::string::npos) << s.message();
}

// ---------------------------------------------------------------------------
// SetNamespacePool (lines 539-541) + the pool/facade live-count branch in
// BuildNamespaceListJson / BuildNamespaceJson (lines 656-661 / 745-751).
// ---------------------------------------------------------------------------

// Wire a real namespace pool (via the shared NsPoolHarness) into the server, admit a NS in
// BOTH the pool and the metadata manager, then list/get it so the per-request
// NamespaceFacade Acquire() succeeds and the live doc/block-count arm runs.
TEST_F(HttpServerLifecycle, SetNamespacePoolLiveCounts) {
    cortrix::test::NsPoolHarness harness(
        std::filesystem::path(root_) / "pool");
    ASSERT_TRUE(harness.Admit("pool-ns").ok());
    server_->SetNamespacePool(&harness.ipool());  // lines 539-541

    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    // Register the same NS name in the metadata manager so list/get find it.
    auto created = cli.Post("/api/v1/namespaces", R"({"name":"pool-ns"})", "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;

    auto list = cli.Get("/api/v1/namespaces");
    ASSERT_TRUE(list);
    EXPECT_EQ(list->status, 200) << list->body;
    auto get = cli.Get("/api/v1/namespaces/pool-ns");
    ASSERT_TRUE(get);
    EXPECT_EQ(get->status, 200) << get->body;
    auto body = json::parse(get->body);
    EXPECT_TRUE(body.contains("doc_count"));
    EXPECT_TRUE(body.contains("block_count"));
}

// ---------------------------------------------------------------------------
// Auth-enabled variant: stats authz-denied arm (lines 596-599).
// ---------------------------------------------------------------------------

class HttpServerLifecycleAuth : public HttpServerLifecycle {
protected:
    void SetUp() override {
        auth_enabled_ = true;
        HttpServerLifecycle::SetUp();
    }
};

// With a namespace authorizer that DENIES, the stats route returns the anti-
// enumeration CX_ERR_NS_UNAUTHORIZED 403 (lines 596-599) before BuildNamespaceJson.
TEST_F(HttpServerLifecycleAuth, NamespaceStatsAuthzDenied) {
    auth_.SetNamespaceAuthorizer(
        [](const AuthContext&, const std::string&) { return false; });  // deny all
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/system/namespaces/anything/stats", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_NS_UNAUTHORIZED");
}

// The same route under auth with NO authorizer installed (ungated) reaches the
// success arm (602-616) after the permission-bit check passes for the admin key.
TEST_F(HttpServerLifecycleAuth, NamespaceStatsAuthSuccess) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto created = cli.Post("/api/v1/namespaces", Admin(), R"({"name":"auth-stats"})",
                            "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    auto res = cli.Get("/api/v1/system/namespaces/auth-stats/stats", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["namespace"], "auth-stats");
}

}  // namespace
}  // namespace cortrix
