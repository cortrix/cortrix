#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/logging/logging.h"
#include "ns_pool_test_helper.h"  // Namespace pool NsPoolHarness replaces MVP NamespaceManager
// [V6] Runtime namespace authorization seam over a real PermissionService
// (the static per-key allow-list / can_access_namespace was removed).
#include "unit/namespace_authz_test_helper.h"

using json = nlohmann::json;

namespace cortrix {
namespace {

class QueryRoutesIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_query_routes_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        // Configure
        config_.server.host = "127.0.0.1";
        config_.server.thread_count = 2;
        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;

        // API key setup
        test_key_ = "test-query-api-key";
        auto hash = ApiKeyAuth::HashKey(test_key_);

        ApiKeyConfig kc;
        kc.key_hash = hash;
        kc.tenant_id = "test-tenant";
        kc.permissions = kPermRead | kPermWrite;
        kc.expires_at = 0;
        config_.auth.api_keys.push_back(kc);

        // Restricted key: a different tenant that owns no test namespace, so its
        // query to "default" is denied by the real PermissionService (403).
        restricted_key_ = "restricted-key";
        auto rhash = ApiKeyAuth::HashKey(restricted_key_);
        ApiKeyConfig rkc;
        rkc.key_hash = rhash;
        rkc.tenant_id = "restricted-tenant";
        rkc.permissions = kPermRead;
        rkc.expires_at = 0;
        config_.auth.api_keys.push_back(rkc);

        auth_.LoadKeys(config_.auth.api_keys);

        // Namespace pool NS resource pool replaces the MVP NamespaceManager +
        // CortrixNamespaceManager (RegisterQueryRoutes now takes INamespacePool&).
        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);

        // [V6] Real PermissionService authz: seed every namespace the admin key
        // (tenant "test-tenant") queries so it is an authorized principal. The
        // ones admitted into the pool in test bodies (testns / rt-ns / gr-ns /
        // ex-ns / ...) return 200/503; "nonexistent-namespace" is authorized but
        // never admitted → facade miss → 404 NOT_FOUND for an authorized caller.
        // "restricted-tenant" owns none of these, so its query to "default" → 403.
        authz_ = std::make_unique<test::NamespaceAuthzHarness>(
            auth_, &harness_->pool(), "test-tenant",
            std::vector<std::string>{"default", "testns", "nonexistent-namespace",
                                     "rt-ns", "gr-ns", "ex-ns", "ex1-ns", "exf-ns"});

        // Initialize embedder and classifier
        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();

        LlmConfig llm_config;
        classifier_ = std::make_unique<IntentClassifier>(llm_config);
        fusion_ = std::make_unique<RRFFusion>(60);

        // Setup server with OS-assigned port
        svr_ = std::make_unique<httplib::Server>();
        RegisterQueryRoutes(*svr_, auth_, harness_->ipool(), *embedder_, *classifier_, *fusion_);

        port_ = svr_->bind_to_any_port("127.0.0.1");

        // Start server in background thread
        svr_thread_ = std::thread([this] {
            svr_->listen_after_bind();
        });

        // Wait for server to start
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/query");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        system(("rm -rf " + tmp_dir_).c_str());
    }

    httplib::Headers AuthHeaders(const std::string& key = "") {
        return {{"Authorization", "Bearer " + (key.empty() ? test_key_ : key)}};
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<test::NamespaceAuthzHarness> authz_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;

    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string tmp_dir_;
    int port_;
    std::string test_key_;
    std::string restricted_key_;
};

// --- Step 1: Invalid JSON ---

TEST_F(QueryRoutesIntegrationTest, InvalidJson_Returns400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        "not valid json{{{", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
    EXPECT_FALSE(body["error"]["request_id"].get<std::string>().empty());
}

// --- Step 1b: Missing query field ---

TEST_F(QueryRoutesIntegrationTest, MissingQueryField_Returns400) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["namespace"] = "default";
    // Missing "query" field

    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// --- Step 2: Validation failure ---

TEST_F(QueryRoutesIntegrationTest, EmptyQuery_Returns400) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "";
    req_body["namespace"] = "default";

    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    // Either parse or validate should catch empty query
    EXPECT_GE(res->status, 400);
    EXPECT_LT(res->status, 500);
}

// --- Step 3: Namespace access denied ---

TEST_F(QueryRoutesIntegrationTest, NamespaceAccessDenied_Returns403) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "test query";
    req_body["namespace"] = "default";

    auto res = cli.Post("/api/v1/query", AuthHeaders(restricted_key_),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);

    auto body = json::parse(res->body);
    // Anti-enumeration (cross-NS query issue 2.6): denied/not-found share the canonical
    // CX_ERR_NS_UNAUTHORIZED identity, and the namespace name is never echoed.
    EXPECT_EQ(body["error"]["code"], "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_EQ(body["error"]["message"], "CX_ERR_NS_UNAUTHORIZED");
    // Per API_GATEWAY_DESIGN.md section 2.5.1: all error responses must include timestamp
    EXPECT_TRUE(body["error"].contains("timestamp"));
}

// --- Step 4: Namespace not found ---

TEST_F(QueryRoutesIntegrationTest, NamespaceNotFound_Returns404) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "test query";
    req_body["namespace"] = "nonexistent-namespace";

    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

// --- No auth header ---

TEST_F(QueryRoutesIntegrationTest, NoAuth_Returns401) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "test query";
    req_body["namespace"] = "default";

    auto res = cli.Post("/api/v1/query",
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// --- Valid query to existing namespace ---
// This test creates a namespace first, then queries it.

TEST_F(QueryRoutesIntegrationTest, ValidQuery_ExistingNamespace_Returns200) {
    // First create a namespace so it exists (admit into the namespace pool)
    harness_->Admit("testns");

    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "find documents about testing";
    req_body["namespace"] = "testns";
    req_body["top_k"] = 5;
    req_body["timeout_ms"] = 5000;

    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    // The query should succeed (even if results are empty since namespace is empty)
    // Or it might return 503 if all routes fail on empty namespace
    EXPECT_TRUE(res->status == 200 || res->status == 503);

    auto body = json::parse(res->body);
    if (res->status == 200) {
        EXPECT_TRUE(body.contains("data") || body.contains("results") || body.contains("meta"));
    } else {
        // 503 = L3 all routes failed (expected for empty namespace)
        EXPECT_EQ(body["error"]["code"], "UNAVAILABLE");
    }
}

// ── query routing ?route enum validation (Agent-friendly error) ──────────────

// An invalid ?route query-string token is rejected with 400 + the query routing
// CX_ERR_ROUTER_FORCE_ROUTE_INVALID body before any namespace acquisition.
TEST_F(QueryRoutesIntegrationTest, InvalidRouteParam_Returns400F39) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "test";
    req_body["namespace"] = "default";
    auto res = cli.Post("/api/v1/query?route=bogus", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_ROUTER_FORCE_ROUTE_INVALID");
    // structured_data echoes the offending value (machine-readable per GEN-Agent).
    EXPECT_EQ(body["error"]["structured_data"]["invalid_route_value"], "bogus");
}

// An invalid ?route value carried in the JSON body (not the query string) is also
// rejected — covers the body-value branch of ApplyRouteGranularityParams' absence.
TEST_F(QueryRoutesIntegrationTest, InvalidRouteInBody_Returns400F39) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "test";
    req_body["namespace"] = "default";
    req_body["route"] = "nonsense";
    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_ROUTER_FORCE_ROUTE_INVALID");
}

// The query string wins over the body: a valid body route + an invalid
// query-string route → rejected (proves the override precedence).
TEST_F(QueryRoutesIntegrationTest, QueryStringRouteOverridesBody) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "test";
    req_body["namespace"] = "default";
    req_body["route"] = "simple";  // valid in body
    auto res = cli.Post("/api/v1/query?route=bogus", AuthHeaders(),  // invalid overrides
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_ROUTER_FORCE_ROUTE_INVALID");
    EXPECT_EQ(body["error"]["structured_data"]["invalid_route_value"], "bogus");
}

// Each valid route token passes the IsValidRoute gate (auto/simple/complex/chat).
TEST_F(QueryRoutesIntegrationTest, ValidRouteTokensPassValidation) {
    harness_->Admit("rt-ns");
    httplib::Client cli("127.0.0.1", port_);
    for (const char* rt : {"auto", "simple", "complex", "chat"}) {
        json req_body;
        req_body["query"] = "test";
        req_body["namespace"] = "rt-ns";
        auto res = cli.Post(std::string("/api/v1/query?route=") + rt, AuthHeaders(),
                            req_body.dump(), "application/json");
        ASSERT_TRUE(res) << rt;
        // Past validation → reaches the pipeline; 200 or 503 (empty ns), never 400.
        EXPECT_TRUE(res->status == 200 || res->status == 503)
            << "route=" << rt << " status=" << res->status;
    }
}

// ── doc summary ?granularity enum validation ─────────────────────────────────

// An invalid granularity is a generic InvalidArgument 400 (no doc summary request-param
// error identity), checked before namespace acquisition.
TEST_F(QueryRoutesIntegrationTest, InvalidGranularityParam_Returns400) {
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "test";
    req_body["namespace"] = "default";
    auto res = cli.Post("/api/v1/query?granularity=weird", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
}

// Each valid granularity token passes the IsValidGranularity gate.
TEST_F(QueryRoutesIntegrationTest, ValidGranularityTokensPassValidation) {
    harness_->Admit("gr-ns");
    httplib::Client cli("127.0.0.1", port_);
    for (const char* g : {"auto", "chunk", "doc", "both"}) {
        json req_body;
        req_body["query"] = "test";
        req_body["namespace"] = "gr-ns";
        auto res = cli.Post(std::string("/api/v1/query?granularity=") + g, AuthHeaders(),
                            req_body.dump(), "application/json");
        ASSERT_TRUE(res) << g;
        EXPECT_TRUE(res->status == 200 || res->status == 503)
            << "granularity=" << g << " status=" << res->status;
    }
}

// ── ?explain query-string override + explain node assembly ───────────────────

// explain=true via the query string surfaces the QueryContext explain node with
// the resolved granularity echoed back (Step 8 explain-assembly branch).
TEST_F(QueryRoutesIntegrationTest, ExplainTrueAttachesExplainNode) {
    harness_->Admit("ex-ns");
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "explain me";
    req_body["namespace"] = "ex-ns";
    auto res = cli.Post("/api/v1/query?explain=true&granularity=doc", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    if (res->status == 200) {
        auto body = json::parse(res->body);
        ASSERT_TRUE(body.contains("explain")) << "explain=true must attach the node";
        EXPECT_EQ(body["explain"]["granularity"], "doc");
    } else {
        // Empty namespace can degrade to L3 (503) before explain assembly; tolerated.
        EXPECT_EQ(res->status, 503);
    }
}

// explain truthy "1" is accepted (the alternate truthy token branch).
TEST_F(QueryRoutesIntegrationTest, ExplainOneIsTruthy) {
    harness_->Admit("ex1-ns");
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "explain me";
    req_body["namespace"] = "ex1-ns";
    auto res = cli.Post("/api/v1/query?explain=1", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    if (res->status == 200) {
        auto body = json::parse(res->body);
        EXPECT_TRUE(body.contains("explain"));
    } else {
        EXPECT_EQ(res->status, 503);
    }
}

// A non-truthy explain token (e.g. "false") leaves explain off — the explain node
// is absent on a 200 response (the falsey branch of ApplyExplainParam).
TEST_F(QueryRoutesIntegrationTest, ExplainFalseOmitsExplainNode) {
    harness_->Admit("exf-ns");
    httplib::Client cli("127.0.0.1", port_);
    json req_body;
    req_body["query"] = "no explain";
    req_body["namespace"] = "exf-ns";
    auto res = cli.Post("/api/v1/query?explain=false", AuthHeaders(),
                        req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    if (res->status == 200) {
        auto body = json::parse(res->body);
        EXPECT_FALSE(body.contains("explain"));
    } else {
        EXPECT_EQ(res->status, 503);
    }
}

}  // namespace
}  // namespace cortrix
