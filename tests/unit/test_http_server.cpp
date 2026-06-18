#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <regex>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"
#include "cortrix/catalog/i_ns_router.h"      // INSRouter + NSMetadata/UnitDescriptor/BatchResult (detail test)
#include "cortrix/common/version.h"
#include "cortrix/observability/operation_logger.h"

#include <memory>
#include <mutex>
#include <vector>

namespace cortrix {
namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// GenerateRequestId tests
// ---------------------------------------------------------------------------

TEST(GenerateRequestIdTest, ReturnsHexString32Chars) {
    std::string id = GenerateRequestId();
    EXPECT_EQ(id.size(), 32u);
    // All chars should be hexadecimal
    for (char c : id) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "Non-hex char found: " << c;
    }
}

TEST(GenerateRequestIdTest, TwoCallsReturnDifferentIds) {
    std::string id1 = GenerateRequestId();
    std::string id2 = GenerateRequestId();
    EXPECT_NE(id1, id2);
}

TEST(GenerateRequestIdTest, ThousandUniqueIds) {
    std::set<std::string> ids;
    for (int i = 0; i < 1000; ++i) {
        ids.insert(GenerateRequestId());
    }
    EXPECT_EQ(ids.size(), 1000u);
}

// ---------------------------------------------------------------------------
// WriteJsonError tests
// ---------------------------------------------------------------------------

TEST(WriteJsonErrorTest, SetsStatusAndContentType) {
    httplib::Response res;
    WriteJsonError(res, Status::NotFound("missing resource"), "req-123");

    EXPECT_EQ(res.status, 404);
    EXPECT_EQ(res.get_header_value("Content-Type"), "application/json");
}

TEST(WriteJsonErrorTest, IncludesErrorCodeAndMessage) {
    httplib::Response res;
    WriteJsonError(res, Status::NotFound("item xyz"), "req-abc");

    auto body = json::parse(res.body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
    EXPECT_EQ(body["error"]["message"], "item xyz");
}

TEST(WriteJsonErrorTest, IncludesRequestId) {
    httplib::Response res;
    WriteJsonError(res, Status::Internal("fail"), "request-id-42");

    auto body = json::parse(res.body);
    EXPECT_EQ(body["error"]["request_id"], "request-id-42");
    EXPECT_EQ(res.get_header_value("X-Request-Id"), "request-id-42");
}

TEST(WriteJsonErrorTest, EmptyRequestIdOmitsField) {
    httplib::Response res;
    WriteJsonError(res, Status::Internal("fail"), "");

    auto body = json::parse(res.body);
    EXPECT_FALSE(body["error"].contains("request_id"));
}

TEST(WriteJsonErrorTest, IncludesTimestamp) {
    httplib::Response res;
    WriteJsonError(res, Status::Internal("fail"), "req-ts");

    auto body = json::parse(res.body);
    EXPECT_TRUE(body["error"].contains("timestamp"));
    std::string ts = body["error"]["timestamp"];
    // Should match ISO 8601: YYYY-MM-DDTHH:MM:SS.mmmZ
    std::regex iso_regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)");
    EXPECT_TRUE(std::regex_match(ts, iso_regex))
        << "Timestamp '" << ts << "' does not match ISO 8601 format";
}

TEST(WriteJsonErrorTest, AllStatusCodesMapCorrectly) {
    // Test each status code maps to the correct HTTP status
    struct TestCase {
        Status status;
        int expected_http;
        std::string expected_code;
    };
    std::vector<TestCase> cases = {
        {Status::InvalidArgument("bad"), 400, "INVALID_ARGUMENT"},
        {Status::Unauthenticated("Missing Authorization header"), 401, "UNAUTHORIZED"},
        {Status::Unauthenticated("Invalid API key"), 401, "INVALID_API_KEY"},
        {Status::Unauthenticated("API key expired"), 401, "EXPIRED_API_KEY"},
        {Status::PermissionDenied("WRITE permission required"), 403, "FORBIDDEN"},
        {Status::PermissionDenied("Access denied for namespace 'hr'"), 403, "NAMESPACE_DENIED"},
        {Status::NotFound("not found"), 404, "NOT_FOUND"},
        {Status::AlreadyExists("dup"), 409, "CONFLICT"},
        {Status::Internal("err"), 500, "INTERNAL_ERROR"},
        {Status::Unavailable("down"), 503, "SERVICE_UNAVAILABLE"},
    };

    for (const auto& tc : cases) {
        httplib::Response res;
        WriteJsonError(res, tc.status, "test-req");
        EXPECT_EQ(res.status, tc.expected_http)
            << "Failed for status: " << tc.status.message();
        auto body = json::parse(res.body);
        EXPECT_EQ(body["error"]["code"], tc.expected_code)
            << "Failed for status: " << tc.status.message();
    }
}

// ---------------------------------------------------------------------------
// WriteJsonResponse tests
// ---------------------------------------------------------------------------

TEST(WriteJsonResponseTest, SetsStatusAndContent) {
    httplib::Response res;
    json body = {{"name", "test"}, {"count", 42}};
    WriteJsonResponse(res, 200, body, "req-001");

    EXPECT_EQ(res.status, 200);
    auto parsed = json::parse(res.body);
    EXPECT_EQ(parsed["name"], "test");
    EXPECT_EQ(parsed["count"], 42);
}

TEST(WriteJsonResponseTest, SetsRequestIdHeader) {
    httplib::Response res;
    json body = {{"ok", true}};
    WriteJsonResponse(res, 201, body, "my-request-id");

    EXPECT_EQ(res.get_header_value("X-Request-Id"), "my-request-id");
}

TEST(WriteJsonResponseTest, EmptyRequestIdNoHeader) {
    httplib::Response res;
    json body = {{"ok", true}};
    WriteJsonResponse(res, 200, body, "");

    EXPECT_TRUE(res.get_header_value("X-Request-Id").empty());
}

TEST(WriteJsonResponseTest, VariousHttpCodes) {
    for (int code : {200, 201, 202, 204, 400, 404, 500}) {
        httplib::Response res;
        json body = {{"status", code}};
        WriteJsonResponse(res, code, body, "");
        EXPECT_EQ(res.status, code);
    }
}

// ---------------------------------------------------------------------------
// CortrixHttpServer lifecycle tests (requires running server)
// ---------------------------------------------------------------------------

class HttpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_http_srv_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        config_.server.host = "127.0.0.1";
        config_.server.port = 19100 + (getpid() % 500);
        config_.server.thread_count = 2;
        config_.auth.enabled = false;
        config_.ns.data_dir = tmp_dir_;

        ns_mgr_ = std::make_unique<NamespaceManager>(tmp_dir_);
        ns_mgr_->Init();

        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
    }

    void TearDown() override {
        if (server_) {
            server_->Stop();
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        system(("rm -rf " + tmp_dir_).c_str());
    }

    void StartServer() {
        server_->RegisterRoutes();
        server_thread_ = std::thread([this] { server_->Start(); });
        // Wait for server to be ready
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

TEST_F(HttpServerTest, HealthEndpointReturnsHealthy) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/health");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["status"], "healthy");
    EXPECT_EQ(body["version"], cortrix::kCortrixVersion);  // [P5] version SoT (1.0.0)
    EXPECT_TRUE(body.contains("uptime_seconds"));
    EXPECT_TRUE(body.contains("components"));
}

TEST_F(HttpServerTest, HealthEndpointHasRequestIdHeader) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/health");

    ASSERT_TRUE(res);
    std::string req_id = res->get_header_value("X-Request-Id");
    EXPECT_EQ(req_id.size(), 32u);
}

TEST_F(HttpServerTest, HealthEndpointUptimeIncreases) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res1 = cli.Get("/api/v1/health");
    ASSERT_TRUE(res1);
    auto body1 = json::parse(res1->body);
    int64_t uptime1 = body1["uptime_seconds"];

    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto res2 = cli.Get("/api/v1/health");
    ASSERT_TRUE(res2);
    auto body2 = json::parse(res2->body);
    int64_t uptime2 = body2["uptime_seconds"];

    EXPECT_GE(uptime2, uptime1);
}

TEST_F(HttpServerTest, UnmatchedRouteReturns404Json) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/nonexistent");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
    EXPECT_TRUE(body["error"].contains("request_id"));
}

TEST_F(HttpServerTest, StopIsIdempotent) {
    StartServer();
    // Stop twice should not crash
    server_->Stop();
    server_->Stop();
    // No assertion needed, just verifying no crash
}

TEST_F(HttpServerTest, ServerReferenceAccessible) {
    auto& svr = server_->server();
    // Should compile and return a valid reference
    EXPECT_TRUE(true);  // Just verifying it compiles and doesn't throw
    (void)svr;
}

// ---------------------------------------------------------------------------
// Namespace CRUD via HTTP (auth disabled)
// ---------------------------------------------------------------------------

TEST_F(HttpServerTest, CreateNamespaceNoAuth) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Post("/api/v1/namespaces", R"({"name":"test-ns"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["name"], "test-ns");
    EXPECT_TRUE(body.contains("created_at"));
}

// [F18a §9.1 NamespaceManager site] A capturing operation logger. The route runs on
// a server worker thread, so Log can be called concurrently — guard the vector.
class CapturingNsOpLogger : public observability::IOperationLogger {
public:
    std::mutex mu;
    std::vector<observability::OperationLogEntry> logged;
    void Log(const observability::OperationLogEntry& e,
             const observability::TraceContext*) override {
        std::lock_guard<std::mutex> lk(mu);
        logged.push_back(e);
    }
    void BatchLog(const std::vector<observability::OperationLogEntry>&,
                  const observability::TraceContext*) override {}
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext*) override { return Status::Internal("unused"); }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }
};

TEST_F(HttpServerTest, NamespaceCreateDeleteEmitOperationLog) {
    auto op_logger = std::make_shared<CapturingNsOpLogger>();
    server_->SetOperationLogger(op_logger.get());  // inject before the routes run
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto created = cli.Post("/api/v1/namespaces", R"({"name":"audit-ns"})",
                            "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201);

    auto deleted = cli.Delete("/api/v1/namespaces/audit-ns");
    ASSERT_TRUE(deleted);
    ASSERT_EQ(deleted->status, 204);

    std::lock_guard<std::mutex> lk(op_logger->mu);
    ASSERT_EQ(op_logger->logged.size(), 2u);
    EXPECT_EQ(op_logger->logged[0].action, "ns_create");
    EXPECT_EQ(op_logger->logged[0].resource_type, "namespace");
    EXPECT_EQ(op_logger->logged[0].resource_id, "audit-ns");
    EXPECT_EQ(op_logger->logged[0].namespace_id, "audit-ns");
    EXPECT_EQ(op_logger->logged[1].action, "ns_delete");
    EXPECT_EQ(op_logger->logged[1].resource_id, "audit-ns");
}

TEST_F(HttpServerTest, NamespaceCreateFailureDoesNotEmitOperationLog) {
    auto op_logger = std::make_shared<CapturingNsOpLogger>();
    server_->SetOperationLogger(op_logger.get());
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    // Missing 'name' → 400 before the manager runs → success-only log stays empty.
    auto res = cli.Post("/api/v1/namespaces", R"({"foo":"bar"})", "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);

    std::lock_guard<std::mutex> lk(op_logger->mu);
    EXPECT_TRUE(op_logger->logged.empty());
}

TEST_F(HttpServerTest, CreateNamespaceInvalidJson) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Post("/api/v1/namespaces", "not json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(HttpServerTest, CreateNamespaceMissingName) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Post("/api/v1/namespaces", R"({"foo":"bar"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HttpServerTest, CreateNamespaceNameNotString) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Post("/api/v1/namespaces", R"({"name":123})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HttpServerTest, ListNamespacesNoAuth) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Get("/api/v1/namespaces");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body["namespaces"].is_array());
    EXPECT_TRUE(body.contains("total"));
    // At minimum, "default" namespace should exist
    EXPECT_GE(body["total"].get<int>(), 1);
}

TEST_F(HttpServerTest, GetNamespaceByName) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    // Create first
    cli.Post("/api/v1/namespaces", R"({"name":"get-test"})", "application/json");

    auto res = cli.Get("/api/v1/namespaces/get-test");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["name"], "get-test");
}

TEST_F(HttpServerTest, GetNonexistentNamespace) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Get("/api/v1/namespaces/does-not-exist");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

// A catalog-backed (ns_router_) detail response must serialize the FULL NSMetadata
// (F12 §4.1) — not just name/created/updated/dc/bc. Regression for the F12 Major
// under-serialization (Scott P0 root cause: View Details dropped tenant / isolation
// / visibility / status / clone lineage). Optionals are stable keys (null on absent).
namespace {
class DetailFakeNSRouter : public cortrix::catalog::INSRouter {
public:
    cortrix::catalog::NSMetadata md;  // returned verbatim by GetNamespace
    cortrix::Result<std::vector<cortrix::catalog::UnitDescriptor>> LookupUnits(
        const std::string&) const override {
        return std::vector<cortrix::catalog::UnitDescriptor>{};
    }
    cortrix::Result<cortrix::catalog::UnitDescriptor> GetActiveUnit(
        const std::string&) const override {
        return cortrix::catalog::UnitDescriptor{};
    }
    cortrix::Result<cortrix::catalog::NSMetadata> GetNamespace(
        const std::string& ns) const override {
        if (ns != md.name) {
            return cortrix::Status::NotFound("CX_ERR_NS_NOT_FOUND: " + ns);
        }
        return md;
    }
    cortrix::Result<cortrix::catalog::BatchResult<std::string>> ListNamespaces(
        const cortrix::catalog::ListNamespacesOptions&) const override {
        cortrix::catalog::BatchResult<std::string> b;
        b.results.push_back(md.name);
        return b;
    }
    cortrix::Status CreateNamespace(const cortrix::catalog::NSMetadata&) override {
        return cortrix::Status::Ok();
    }
    cortrix::Status DeleteNamespace(const std::string&) override {
        return cortrix::Status::Ok();
    }
};
}  // namespace

TEST_F(HttpServerTest, GetNamespaceDetailSerializesFullNSMetadata) {
    DetailFakeNSRouter router;
    router.md.namespace_id = "ns_01H";
    router.md.name = "detail-ns";
    router.md.tenant_id = "tenant_acme";
    router.md.isolation_mode = "isolated";
    router.md.visibility = "private";
    router.md.owner_user_id = "user_42";
    router.md.status = "active";
    router.md.created_at = 1700000000000LL;
    // cloned_from_ns_id / clone_type / cloned_at left unset → must serialize as null.
    server_->SetNamespaceRouter(&router);

    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/namespaces/detail-ns");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["name"], "detail-ns");
    EXPECT_EQ(body["namespace_id"], "ns_01H");
    EXPECT_EQ(body["tenant_id"], "tenant_acme");
    EXPECT_EQ(body["isolation_mode"], "isolated");
    EXPECT_EQ(body["visibility"], "private");
    EXPECT_EQ(body["owner_user_id"], "user_42");
    EXPECT_EQ(body["status"], "active");
    EXPECT_EQ(body["created_at"], 1700000000000LL);
    // No updated_at column in F12 §4.1 → mirrors created_at.
    EXPECT_EQ(body["updated_at"], body["created_at"]);
    // Stable-shape optionals: present-but-null when the NS was never cloned.
    ASSERT_TRUE(body.contains("cloned_from_ns_id"));
    EXPECT_TRUE(body["cloned_from_ns_id"].is_null());
    ASSERT_TRUE(body.contains("clone_type"));
    EXPECT_TRUE(body["clone_type"].is_null());
    ASSERT_TRUE(body.contains("cloned_at"));
    EXPECT_TRUE(body["cloned_at"].is_null());
}

TEST_F(HttpServerTest, DeleteNamespace) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    cli.Post("/api/v1/namespaces", R"({"name":"del-test"})", "application/json");

    auto res = cli.Delete("/api/v1/namespaces/del-test");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_EQ(res->get_header_value("X-Request-Id").size(), 32u);

    // Verify deleted
    res = cli.Get("/api/v1/namespaces/del-test");
    EXPECT_EQ(res->status, 404);
}

TEST_F(HttpServerTest, DeleteNonexistentNamespace) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Delete("/api/v1/namespaces/ghost");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(HttpServerTest, DeleteDefaultNamespaceReturns400) {
    StartServer();
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Delete("/api/v1/namespaces/default");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// ---------------------------------------------------------------------------
// Auth-enabled route tests
// ---------------------------------------------------------------------------

class HttpServerAuthEnabledTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_http_auth_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        test_key_ = "auth-test-key";
        config_.server.host = "127.0.0.1";
        config_.server.port = 19200 + (getpid() % 500);
        config_.server.thread_count = 2;
        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;

        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "test-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        kc.expires_at = 0;
        config_.auth.api_keys.push_back(kc);

        auth_.LoadKeys(config_.auth.api_keys);

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
        server_->Stop();
        if (server_thread_.joinable()) server_thread_.join();
        system(("rm -rf " + tmp_dir_).c_str());
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    std::string tmp_dir_;
    std::string test_key_;
};

TEST_F(HttpServerAuthEnabledTest, NamespaceCreateRequiresAuth) {
    httplib::Client cli("127.0.0.1", config_.server.port);

    // Without auth
    auto res = cli.Post("/api/v1/namespaces", R"({"name":"x"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(HttpServerAuthEnabledTest, NamespaceCreateWithAuth) {
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Post("/api/v1/namespaces", AuthHeaders(),
                        R"({"name":"auth-ns"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);
}

TEST_F(HttpServerAuthEnabledTest, NamespaceListWithAuth) {
    httplib::Client cli("127.0.0.1", config_.server.port);

    auto res = cli.Get("/api/v1/namespaces", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("total"));
}

TEST_F(HttpServerAuthEnabledTest, NamespaceGetWithAuth) {
    httplib::Client cli("127.0.0.1", config_.server.port);

    // Create via auth
    cli.Post("/api/v1/namespaces", AuthHeaders(),
             R"({"name":"get-auth"})", "application/json");

    auto res = cli.Get("/api/v1/namespaces/get-auth", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(HttpServerAuthEnabledTest, NamespaceDeleteWithAuth) {
    httplib::Client cli("127.0.0.1", config_.server.port);

    cli.Post("/api/v1/namespaces", AuthHeaders(),
             R"({"name":"del-auth"})", "application/json");

    auto res = cli.Delete("/api/v1/namespaces/del-auth", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
}

TEST_F(HttpServerAuthEnabledTest, HealthEndpointDoesNotRequireAuth) {
    httplib::Client cli("127.0.0.1", config_.server.port);
    auto res = cli.Get("/api/v1/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

// ---------------------------------------------------------------------------
// NoAuth middleware wrapper tests
// ---------------------------------------------------------------------------

TEST(NoAuthMiddlewareTest, SetsFullPermissions) {
    bool handler_called = false;
    int captured_permissions = 0;
    std::string captured_request_id;

    auto wrapped = NoAuth(
        [&](const httplib::Request&, httplib::Response&, const RequestContext& rctx) {
            handler_called = true;
            captured_permissions = rctx.auth.permissions;
            captured_request_id = rctx.request_id;
        }
    );

    httplib::Request req;
    httplib::Response res;
    wrapped(req, res);

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(captured_permissions, kPermRead | kPermWrite | kPermAdmin);
    EXPECT_EQ(captured_request_id.size(), 32u);
}

}  // namespace
}  // namespace cortrix
