// Route request-validation matrices for the query routing unified query route
// (POST /api/v1/query) and the watcher connector routes. BREADTH coverage of
// malformed JSON, missing/empty required fields, out-of-range top_k/timeout_ms,
// bad ?route / ?granularity enums, missing auth -> 401, namespace authz ->
// 403 / 404, and method-not-allowed / unknown route -> generic 404.
//
// Same in-process harness as test_query_routes.cpp (real httplib server +
// Namespace pool NsPoolHarness + real PermissionService authz + fake embedder/classifier/
// fusion) and test_connector_routes.cpp. No live network / LLM.
//
// Suite/fixture names area-prefixed (QueryRouteValMatrix / ConnRouteValMatrix)
// for global uniqueness.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/config/config.h"
#include "cortrix/connector/dir_watcher_registry.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/spc_manager.h"

#include "ns_pool_test_helper.h"
#include "unit/namespace_authz_test_helper.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// ===========================================================================
// POST /api/v1/query validation
// ===========================================================================
class QueryRouteValMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_qrvm_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        config_.server.host = "127.0.0.1";
        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;

        admin_key_ = "qrvm-admin-key";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        kc.tenant_id = "test-tenant";
        kc.permissions = kPermRead | kPermWrite;
        kc.expires_at = 0;
        config_.auth.api_keys.push_back(kc);

        // A different tenant that owns no test namespace -> 403 on "default".
        restricted_key_ = "qrvm-restricted-key";
        ApiKeyConfig rkc;
        rkc.key_hash = ApiKeyAuth::HashKey(restricted_key_);
        rkc.tenant_id = "restricted-tenant";
        rkc.permissions = kPermRead;
        rkc.expires_at = 0;
        config_.auth.api_keys.push_back(rkc);

        auth_.LoadKeys(config_.auth.api_keys);

        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);
        authz_ = std::make_unique<test::NamespaceAuthzHarness>(
            auth_, &harness_->pool(), "test-tenant",
            std::vector<std::string>{"default", "qns", "missing-ns"});

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        LlmConfig llm_config;
        classifier_ = std::make_unique<IntentClassifier>(llm_config);
        fusion_ = std::make_unique<RRFFusion>(60);

        svr_ = std::make_unique<httplib::Server>();
        RegisterQueryRoutes(*svr_, auth_, harness_->ipool(), *embedder_, *classifier_,
                            *fusion_);
        port_ = svr_->bind_to_any_port("127.0.0.1");
        svr_thread_ = std::thread([this] { svr_->listen_after_bind(); });

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

    httplib::Headers Auth(const std::string& key = "") {
        return {{"Authorization", "Bearer " + (key.empty() ? admin_key_ : key)}};
    }

    // A valid query body to a known namespace (admitted by the test body).
    static std::string Body(const std::string& ns, const std::string& q = "hello") {
        json b;
        b["query"] = q;
        b["namespace"] = ns;
        return b.dump();
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
    std::string admin_key_;
    std::string restricted_key_;
};

// ---- auth matrix ----------------------------------------------------------

TEST_F(QueryRouteValMatrix, NoAuth401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/query", Body("default"), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(QueryRouteValMatrix, UnknownKey401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/query", Auth("ghost"), Body("default"), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(QueryRouteValMatrix, BadScheme401) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h{{"Authorization", "Token " + admin_key_}};
    auto res = cli.Post("/api/v1/query", h, Body("default"), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ---- namespace authz: denied (403) vs not-admitted (404) ------------------

TEST_F(QueryRouteValMatrix, RestrictedTenantDenied403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/query", Auth(restricted_key_), Body("default"),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_TRUE(body["error"].contains("timestamp"));
}

TEST_F(QueryRouteValMatrix, AuthorizedButMissingNamespace404) {
    httplib::Client cli("127.0.0.1", port_);
    // "missing-ns" is authorized for test-tenant but never admitted into the pool.
    auto res = cli.Post("/api/v1/query", Auth(), Body("missing-ns"), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "NOT_FOUND");
}

// ---- malformed JSON matrix (TEST_P) ---------------------------------------

class QueryRouteValBadJson
    : public QueryRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(QueryRouteValBadJson, Returns400InvalidArgument) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/query", Auth(), GetParam(), "application/json");
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 400) << GetParam();
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT") << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Bodies, QueryRouteValBadJson,
    ::testing::Values(std::string("not json{{{"), std::string("}{"),
                      std::string("[1,2,3"), std::string("\"a string\""),
                      std::string("null"), std::string("42"),
                      std::string("true"), std::string("false"),
                      std::string("3.14"), std::string("[]"),
                      std::string("\"\""), std::string("{\"query\":}"),
                      std::string("{\"query\":\"x\""), std::string("{,}"),
                      std::string("undefined"), std::string("NaN"),
                      std::string("{'q':'x'}"), std::string("{\"q\":\"x\",}"),
                      std::string("")));

// ---- missing / empty required fields --------------------------------------

TEST_F(QueryRouteValMatrix, MissingQuery400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "default";
    auto res = cli.Post("/api/v1/query", Auth(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(QueryRouteValMatrix, EmptyQuery400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["query"] = "";
    b["namespace"] = "default";
    auto res = cli.Post("/api/v1/query", Auth(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400);
    EXPECT_LT(res->status, 500);
}

TEST_F(QueryRouteValMatrix, MissingNamespace400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["query"] = "hi";
    auto res = cli.Post("/api/v1/query", Auth(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400);
    EXPECT_LT(res->status, 500);
}

// ---- invalid namespace format matrix (TEST_P) -----------------------------
// namespace must match [a-zA-Z0-9_-]{1,64}; bad forms -> 400 (before authz).

class QueryRouteValBadNs
    : public QueryRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(QueryRouteValBadNs, Returns400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["query"] = "hi";
    b["namespace"] = GetParam();
    auto res = cli.Post("/api/v1/query", Auth(), b.dump(), "application/json");
    ASSERT_TRUE(res) << GetParam();
    EXPECT_GE(res->status, 400) << GetParam();
    EXPECT_LT(res->status, 500) << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Names, QueryRouteValBadNs,
    ::testing::Values(std::string(""), std::string("has space"),
                      std::string("bad/slash"), std::string("dot.name"),
                      std::string("name!"), std::string("name@ns"),
                      std::string("name#hash"), std::string("name$"),
                      std::string("name%20"), std::string("name+plus"),
                      std::string("name:colon"), std::string("name;semi"),
                      std::string("tab\tname"), std::string("name(paren)"),
                      std::string(std::string(65, 'x')),    // too long (>64)
                      std::string(std::string(128, 'y'))));  // far too long

// ---- top_k boundary matrix (TEST_P) ---------------------------------------
// Valid range 1..100. Out-of-range integer top_k -> 400.

struct TopKCase {
    int top_k;
    bool reject;  // true => expect 4xx
};

class QueryRouteValTopK
    : public QueryRouteValMatrix,
      public ::testing::WithParamInterface<TopKCase> {};

TEST_P(QueryRouteValTopK, OutOfRangeRejected) {
    const auto& tc = GetParam();
    harness_->Admit("qns");  // authorized + admitted -> passes authz on accept cases
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["query"] = "hi";
    b["namespace"] = "qns";
    b["top_k"] = tc.top_k;
    auto res = cli.Post("/api/v1/query", Auth(), b.dump(), "application/json");
    ASSERT_TRUE(res) << "top_k=" << tc.top_k;
    if (tc.reject) {
        EXPECT_GE(res->status, 400) << "top_k=" << tc.top_k;
        EXPECT_LT(res->status, 500) << "top_k=" << tc.top_k;
    } else {
        // In range: past validation -> reaches pipeline (200 or 503 on empty ns),
        // never a 4xx.
        EXPECT_TRUE(res->status == 200 || res->status == 503)
            << "top_k=" << tc.top_k << " status=" << res->status;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Values, QueryRouteValTopK,
    ::testing::Values(TopKCase{0, true}, TopKCase{-1, true}, TopKCase{-100, true},
                      TopKCase{101, true}, TopKCase{500, true}, TopKCase{1000000, true},
                      TopKCase{1, false}, TopKCase{50, false}, TopKCase{100, false}));

// ---- timeout_ms boundary matrix (TEST_P) ----------------------------------
// Valid range 100..30000. Out-of-range -> 400.

struct TimeoutCase {
    int timeout_ms;
    bool reject;
};

class QueryRouteValTimeout
    : public QueryRouteValMatrix,
      public ::testing::WithParamInterface<TimeoutCase> {};

TEST_P(QueryRouteValTimeout, OutOfRangeRejected) {
    const auto& tc = GetParam();
    harness_->Admit("qns");
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["query"] = "hi";
    b["namespace"] = "qns";
    b["timeout_ms"] = tc.timeout_ms;
    auto res = cli.Post("/api/v1/query", Auth(), b.dump(), "application/json");
    ASSERT_TRUE(res) << "timeout_ms=" << tc.timeout_ms;
    if (tc.reject) {
        EXPECT_GE(res->status, 400) << "timeout_ms=" << tc.timeout_ms;
        EXPECT_LT(res->status, 500) << "timeout_ms=" << tc.timeout_ms;
    } else {
        EXPECT_TRUE(res->status == 200 || res->status == 503)
            << "timeout_ms=" << tc.timeout_ms << " status=" << res->status;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Values, QueryRouteValTimeout,
    ::testing::Values(TimeoutCase{0, true}, TimeoutCase{99, true}, TimeoutCase{-1, true},
                      TimeoutCase{30001, true}, TimeoutCase{100000, true},
                      TimeoutCase{100, false}, TimeoutCase{5000, false},
                      TimeoutCase{30000, false}));

// ---- bad ?route enum matrix (TEST_P) -> query routing body ----------------

class QueryRouteValBadRoute
    : public QueryRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(QueryRouteValBadRoute, F39RouteInvalid400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/query?route=" + GetParam(), Auth(), Body("default"),
                        "application/json");
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 400) << GetParam();
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_ROUTER_FORCE_ROUTE_INVALID") << GetParam();
    EXPECT_EQ(body["error"]["structured_data"]["invalid_route_value"], GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Values, QueryRouteValBadRoute,
    ::testing::Values(std::string("bogus"), std::string("AUTO"), std::string("simple2"),
                      std::string("fast"), std::string("123"), std::string("none"),
                      std::string("Simple"), std::string("Complex"), std::string("CHAT"),
                      std::string("hybrid"), std::string("default"), std::string("x")));

// ---- bad ?granularity enum matrix (TEST_P) -> generic 400 -----------------

class QueryRouteValBadGran
    : public QueryRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(QueryRouteValBadGran, GranularityInvalid400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/query?granularity=" + GetParam(), Auth(), Body("default"),
                        "application/json");
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 400) << GetParam();
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT") << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Values, QueryRouteValBadGran,
    ::testing::Values(std::string("weird"), std::string("CHUNK"), std::string("docs"),
                      std::string("all"), std::string("1"), std::string("Doc"),
                      std::string("Both"), std::string("paragraph"),
                      std::string("sentence"), std::string("none")));

// ---- method-not-allowed / unknown route -----------------------------------

TEST_F(QueryRouteValMatrix, GetOnQueryGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/query", Auth());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(QueryRouteValMatrix, UnknownQueryRouteGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/query/extra", Auth(), Body("default"), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

// ===========================================================================
// Connector routes validation (auth-disabled CE mode like the existing test)
// ===========================================================================
namespace fs = std::filesystem;

class ConnRouteValMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() / ("cortrix_crvm_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);
        watch_dir_ = temp_dir_ / "watch";
        fs::create_directories(watch_dir_);

        connector_state_.data_dir = (temp_dir_ / "state").string();
        fs::create_directories(connector_state_.data_dir);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        using ::testing::_;
        using ::testing::Return;
        ON_CALL(ns_router_, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));

        spc_ = std::make_unique<StubSPC>();
        auth_ = std::make_unique<ApiKeyAuth>();
        auth_->set_enabled(false);

        port_ = 18900 + (getpid() % 200);
        server_ = std::make_unique<httplib::Server>();
        RegisterConnectorRoutes(*server_, connector_state_, harness_->ipool(), ns_router_,
                                *spc_, *auth_);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    class StubSPC : public SPCManager {
    public:
        StubSPC() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    fs::path temp_dir_;
    fs::path watch_dir_;
    ConnectorState connector_state_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router_;
    std::unique_ptr<StubSPC> spc_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_;
};

// ---- malformed JSON on the two POST bodies (TEST_P) -----------------------

struct ConnPost {
    std::string path;
};

class ConnRouteValBadJson
    : public ConnRouteValMatrix,
      public ::testing::WithParamInterface<ConnPost> {};

TEST_P(ConnRouteValBadJson, Returns400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post(GetParam().path.c_str(), "not-json{{{", "application/json");
    ASSERT_TRUE(res) << GetParam().path;
    EXPECT_EQ(res->status, 400) << GetParam().path << " body=" << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error")) << GetParam().path;
}

INSTANTIATE_TEST_SUITE_P(
    Posts, ConnRouteValBadJson,
    ::testing::Values(ConnPost{"/api/v1/connector/watchers"},
                      ConnPost{"/api/v1/connector/watch"}));

// ---- missing required path field -> 400 -----------------------------------

TEST_F(ConnRouteValMatrix, AddWatcherMissingPath400) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"namespace_name", "default"}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(ConnRouteValMatrix, WatchCompatMissingDataDir400) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"namespace_name", "default"}};
    auto res = cli.Post("/api/v1/connector/watch", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(ConnRouteValMatrix, AddWatcherEmptyTargetNamespaces400) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"path", watch_dir_.string()}, {"target_namespaces", json::array()}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT");
}

// ---- dir-not-found -> 404 (NOT_FOUND, a business body, not a clobber) ------

TEST_F(ConnRouteValMatrix, AddWatcherNonexistentDir404) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"data_dir", "/tmp/cortrix_no_such_dir_crvm_987"},
                {"namespace_name", "default"}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "NOT_FOUND");
}

TEST_F(ConnRouteValMatrix, BrowseNonexistentPath404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/browse?path=/no/such/path/crvm_never");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "NOT_FOUND");
}

// ---- :id not-found on the watcher sub-routes (TEST_P) ---------------------

struct ConnIdRoute {
    std::string method;
    std::string path;
};

class ConnRouteValMissingId
    : public ConnRouteValMatrix,
      public ::testing::WithParamInterface<ConnIdRoute> {
protected:
    httplib::Result Call(httplib::Client& cli) {
        const auto& e = GetParam();
        if (e.method == "DELETE") return cli.Delete(e.path.c_str());
        return cli.Post(e.path.c_str(), "", "application/json");
    }
};

TEST_P(ConnRouteValMissingId, Returns404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = Call(cli);
    ASSERT_TRUE(res) << GetParam().method << " " << GetParam().path;
    EXPECT_EQ(res->status, 404) << GetParam().method << " " << GetParam().path;
}

INSTANTIATE_TEST_SUITE_P(
    MissingId, ConnRouteValMissingId,
    ::testing::Values(
        ConnIdRoute{"DELETE", "/api/v1/connector/watchers/deadbeef"},
        ConnIdRoute{"DELETE", "/api/v1/connector/watchers/deadbeef/namespaces/x"},
        ConnIdRoute{"POST", "/api/v1/connector/watchers/deadbeef/scan"}));

// ---- scan-all with no watchers -> 400 -------------------------------------

TEST_F(ConnRouteValMatrix, ScanAllNoWatchers400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/connector/scan", "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT");
}

// ---- method-not-allowed / unknown route -----------------------------------

TEST_F(ConnRouteValMatrix, DeleteOnWatchersListGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/connector/watchers");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(ConnRouteValMatrix, UnknownConnectorRouteGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/connector/nonexistent");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
}  // namespace cortrix
