#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/agent_trace_writer_impl.h"
#include "cortrix/agent_trace/interaction_sources_schema.h"
#include "cortrix/agent_trace/interactions_handler.h"
#include "cortrix/agent_trace/traces_handler.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/server/routes/observability_routes.h"

// Agent trace HTTP route layer (observability_routes): the 3 GET endpoints over a real
// httplib server (mirrors test_batch_routes' real-server pattern) — auth + happy
// path + permission (admin vs non-admin cross-user) + header-validation warning +
// route-side INVALID_FILTER — plus the InstallCors origin logic (loopback allowed,
// listed origin echoed, non-listed origin rejected, preflight OPTIONS, and the
// look-alike "localhost.evil.com" NOT treated as loopback).
namespace cortrix {
namespace {

using json = nlohmann::json;
using agent_trace::AgentTraceEntry;
using agent_trace::AgentTraceMetrics;
using agent_trace::AgentTraceSchemaProvider;
using agent_trace::AgentTraceWriterImpl;
using agent_trace::InteractionsHandler;
using agent_trace::InteractionSourcesSchemaProvider;
using agent_trace::TracesHandler;

void Exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "") << " :: " << sql;
    sqlite3_free(err);
}

// Minimal IGlobalConfig stub (the writer only reads retention in Cleanup(), which
// these route tests never call).
class StubGlobalConfig : public cortrix::IGlobalConfig {
public:
    Result<std::string> GetString(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<bool> GetBool(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<int> GetInt(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<float> GetFloat(const std::string&) const override { return Status::InvalidArgument("x"); }
    void OnChange(std::function<void(const std::string&)>) override {}
};

class ObservabilityRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        AgentTraceMetrics::Instance().ResetForTest();
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);

        // agent_trace (writer) + interaction_sources (handler) schemas.
        cortrix::catalog::SchemaMigrator m;
        m.Register(&trace_provider_);
        m.Register(&sources_provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        // Real MVP interaction_log (role/content model, id TEXT PK) + blocks probe.
        Exec(db_, R"(
            CREATE TABLE interaction_log (
                id TEXT PRIMARY KEY, session_id TEXT NOT NULL, namespace_name TEXT NOT NULL,
                user_id TEXT, role TEXT NOT NULL, content TEXT NOT NULL, query_type TEXT,
                status TEXT, latency_ms INTEGER DEFAULT 0, metadata_json TEXT,
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
            );)");
        Exec(db_, "CREATE TABLE blocks (block_id TEXT PRIMARY KEY);");

        config_ = std::make_shared<StubGlobalConfig>();
        writer_ = std::make_shared<AgentTraceWriterImpl>(db_, config_);
        traces_ = std::make_unique<TracesHandler>(writer_, db_);
        inter_  = std::make_unique<InteractionsHandler>(db_);

        // Seed: session/interaction owned by alice.
        Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content) "
                  "VALUES('int-1','alice-sess','sales','alice','user','what is X?');");
        Exec(db_, "INSERT INTO blocks(block_id) VALUES('b1'),('b2');");
        Exec(db_, "INSERT INTO interaction_sources(interaction_id, source_block_id, source_type, relevance_score, snippet) "
                  "VALUES('int-1','b1','block',0.9,'top snippet'),"
                  "      ('int-1','b2','memory',0.5,'mid snippet');");
        for (int i = 0; i < 3; ++i) {
            AgentTraceEntry e;
            e.session_id = "alice-sess";
            e.method = "cortrix_query";
            e.source = "mcp";
            e.duration_ms = 10 + i;
            e.created_at = 1000 + i;
            writer_->Write(e);
        }

        // Auth: user_id == tenant_id (MVP). alice/mallory = Read; root = Read+Admin.
        auth_ = std::make_unique<ApiKeyAuth>();
        alice_key_   = "alice-key";
        mallory_key_ = "mallory-key";
        admin_key_   = "admin-key";
        auth_->LoadKeys({
            MakeKey(alice_key_,   "alice",   kPermRead),
            MakeKey(mallory_key_, "mallory", kPermRead),
            MakeKey(admin_key_,   "root",    kPermRead | kPermAdmin),
        });

        // Real server with the routes + CORS (listed origin = https://app.example.com).
        port_ = 19200 + (getpid() % 500);
        server_ = std::make_unique<httplib::Server>();
        RegisterObservabilityRoutes(*server_, *traces_, *inter_, *auth_);
        InstallCors(*server_, {"https://app.example.com"});
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        traces_.reset();
        inter_.reset();
        writer_.reset();
        if (db_) sqlite3_close(db_);
        AgentTraceMetrics::Instance().ResetForTest();
    }

    static ApiKeyConfig MakeKey(const std::string& plaintext, const std::string& tenant,
                                int perms) {
        ApiKeyConfig k;
        k.key_hash = ApiKeyAuth::HashKey(plaintext);
        k.tenant_id = tenant;       // -> AuthContext.user_id (MVP user_id == tenant_id)
        k.permissions = perms;
        return k;
    }

    httplib::Headers Bearer(const std::string& key) {
        return {{"Authorization", "Bearer " + key}};
    }

    AgentTraceSchemaProvider trace_provider_;
    InteractionSourcesSchemaProvider sources_provider_;
    std::shared_ptr<StubGlobalConfig> config_;
    std::shared_ptr<AgentTraceWriterImpl> writer_;
    std::unique_ptr<TracesHandler> traces_;
    std::unique_ptr<InteractionsHandler> inter_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string alice_key_, mallory_key_, admin_key_;
    sqlite3* db_ = nullptr;
};

// ---- auth gate -------------------------------------------------------------

TEST_F(ObservabilityRoutesTest, NoAuthRejected) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ---- GET /traces/{session_id} ---------------------------------------------

TEST_F(ObservabilityRoutesTest, OwnerReadsOwnSession) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess", Bearer(alice_key_));
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["session_id"], "alice-sess");
    EXPECT_EQ(body["trace_count"], 3);
    EXPECT_EQ(body["traces"].size(), 3u);
    EXPECT_EQ(body["traces"][0]["method"], "cortrix_query");
    EXPECT_EQ(body["traces"][0]["source"], "mcp");
    // null optional columns are present as JSON null (stable schema).
    EXPECT_TRUE(body["traces"][0]["error_code"].is_null());
    EXPECT_EQ(body["meta"]["total_count"], 3);
    EXPECT_FALSE(body["meta"]["response_size_warning"].get<bool>());
}

TEST_F(ObservabilityRoutesTest, NonAdminCrossUserUnauthorized) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess", Bearer(mallory_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_TRACE_UNAUTHORIZED");
    EXPECT_EQ(body["error"]["category"], "auth");
    EXPECT_FALSE(body["error"]["retryable"].get<bool>());
    // §9.2 structured_data required key for UNAUTHORIZED.
    EXPECT_EQ(body["error"]["structured_data"]["required_role"], "admin");
}

TEST_F(ObservabilityRoutesTest, AdminCrossUserReadsSession) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess", Bearer(admin_key_));
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["trace_count"], 3);
}

TEST_F(ObservabilityRoutesTest, AdminNonexistentSessionNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/ghost-sess", Bearer(admin_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_TRACE_SESSION_NOT_FOUND");
    EXPECT_EQ(body["error"]["structured_data"]["session_id"], "ghost-sess");
}

TEST_F(ObservabilityRoutesTest, InvalidLimitParamIsInvalidFilter) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess?limit=abc", Bearer(alice_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_TRACE_INVALID_FILTER");
    EXPECT_EQ(body["error"]["structured_data"]["invalid_field"], "limit");
    // value_preview echoes the offending value (PII-guarded to 100 chars).
    EXPECT_EQ(body["error"]["structured_data"]["value_preview"], "abc");
}

TEST_F(ObservabilityRoutesTest, InvalidStatusParamIsInvalidFilter) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess?status=bogus", Bearer(alice_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["structured_data"]["invalid_field"], "status");
}

// out-of-range limit is rejected by the writer (handler) and surfaces as the same
// CX_ERR_TRACE_INVALID_FILTER token through the route.
TEST_F(ObservabilityRoutesTest, OutOfRangeLimitFromHandlerIsInvalidFilter) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess?limit=9999", Bearer(alice_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_TRACE_INVALID_FILTER");
}

// ---- header validation (§6.1, topic 4) ------------------------------------

TEST_F(ObservabilityRoutesTest, InvalidIdentityHeaderWarnsButDoesNotReject) {
    httplib::Client cli("127.0.0.1", port_);
    // A space is outside the [a-zA-Z0-9_.:/-] whitelist -> invalid header.
    httplib::Headers h = Bearer(alice_key_);
    h.insert({"X-Session-Id", "bad id with spaces"});
    auto res = cli.Get("/api/v1/traces/alice-sess", h);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);  // ignored, not rejected
    EXPECT_EQ(res->get_header_value("X-Cortrix-Header-Warning"), "invalid-format");
}

TEST_F(ObservabilityRoutesTest, ValidIdentityHeaderNoWarning) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h = Bearer(alice_key_);
    h.insert({"X-Session-Id", "sess-abc_123"});
    h.insert({"X-Agent-Id", "agent.v2"});
    auto res = cli.Get("/api/v1/traces/alice-sess", h);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(res->get_header_value("X-Cortrix-Header-Warning").empty());
}

// ---- GET /interactions/{id}/sources ---------------------------------------

TEST_F(ObservabilityRoutesTest, SourcesOwnerHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions/int-1/sources", Bearer(alice_key_));
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["interaction_id"], "int-1");
    EXPECT_EQ(body["source_count"], 2);
    ASSERT_EQ(body["sources"].size(), 2u);
    EXPECT_EQ(body["sources"][0]["source_id"], "b1");        // relevance DESC
    EXPECT_EQ(body["sources"][0]["source_type"], "block");
    // CE shape carries NO highlight_ranges.
    EXPECT_FALSE(body["sources"][0].contains("highlight_ranges"));
}

TEST_F(ObservabilityRoutesTest, SourcesMissingInteractionNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions/nope/sources", Bearer(alice_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_TRACE_INTERACTION_NOT_FOUND");
    EXPECT_EQ(body["error"]["structured_data"]["interaction_id"], "nope");
}

TEST_F(ObservabilityRoutesTest, SourcesNonAdminCrossUserUnauthorized) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions/int-1/sources", Bearer(mallory_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_TRACE_UNAUTHORIZED");
}

// ---- GET /interactions -----------------------------------------------------

TEST_F(ObservabilityRoutesTest, ListOwnInteractions) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions", Bearer(alice_key_));
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    ASSERT_EQ(body["interactions"].size(), 1u);
    EXPECT_EQ(body["interactions"][0]["interaction_id"], "int-1");
    EXPECT_EQ(body["interactions"][0]["user_id"], "alice");
    EXPECT_EQ(body["interactions"][0]["namespace_id"], "sales");
    EXPECT_EQ(body["interactions"][0]["query_text"], "what is X?");  // full, not truncated
    EXPECT_EQ(body["meta"]["total_count"], 1);
    EXPECT_FALSE(body["meta"]["has_next"].get<bool>());
}

TEST_F(ObservabilityRoutesTest, ListNonAdminCrossUserUnauthorized) {
    httplib::Client cli("127.0.0.1", port_);
    // mallory asking for alice's rows -> anti-leak UNAUTHORIZED.
    auto res = cli.Get("/api/v1/interactions?user_id=alice", Bearer(mallory_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_TRACE_UNAUTHORIZED");
}

TEST_F(ObservabilityRoutesTest, ListInvalidSortOrderIsInvalidFilter) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions?sort_order=sideways", Bearer(alice_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["structured_data"]["invalid_field"], "sort_order");
}

// ---- CORS ------------------------------------------------------------------

TEST_F(ObservabilityRoutesTest, CorsLoopbackOriginEchoed) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h = Bearer(alice_key_);
    h.insert({"Origin", "http://localhost:5173"});
    auto res = cli.Get("/api/v1/interactions", h);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->get_header_value("Access-Control-Allow-Origin"), "http://localhost:5173");
    EXPECT_EQ(res->get_header_value("Vary"), "Origin");
}

TEST_F(ObservabilityRoutesTest, CorsListedOriginEchoed) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h = Bearer(alice_key_);
    h.insert({"Origin", "https://app.example.com"});
    auto res = cli.Get("/api/v1/interactions", h);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->get_header_value("Access-Control-Allow-Origin"), "https://app.example.com");
}

TEST_F(ObservabilityRoutesTest, CorsUnlistedOriginRejected) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h = Bearer(alice_key_);
    h.insert({"Origin", "https://evil.com"});
    auto res = cli.Get("/api/v1/interactions", h);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);  // request still served (CORS is a browser gate)
    EXPECT_TRUE(res->get_header_value("Access-Control-Allow-Origin").empty());
}

// A look-alike host must NOT be treated as loopback.
TEST_F(ObservabilityRoutesTest, CorsLoopbackLookalikeRejected) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h = Bearer(alice_key_);
    h.insert({"Origin", "http://localhost.evil.com"});
    auto res = cli.Get("/api/v1/interactions", h);
    ASSERT_TRUE(res);
    EXPECT_TRUE(res->get_header_value("Access-Control-Allow-Origin").empty());
}

TEST_F(ObservabilityRoutesTest, CorsPreflightOptionsAllowedOrigin) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h{{"Origin", "https://app.example.com"},
                       {"Access-Control-Request-Method", "GET"}};
    auto res = cli.Options("/api/v1/interactions", h);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_EQ(res->get_header_value("Access-Control-Allow-Origin"), "https://app.example.com");
    EXPECT_NE(res->get_header_value("Access-Control-Allow-Methods").find("OPTIONS"),
              std::string::npos);
    // The agent trace identity headers are in the allow-list.
    const std::string allow_headers = res->get_header_value("Access-Control-Allow-Headers");
    EXPECT_NE(allow_headers.find("X-Session-Id"), std::string::npos);
    EXPECT_NE(allow_headers.find("X-Trace-Id"), std::string::npos);
    EXPECT_NE(allow_headers.find("X-Agent-Id"), std::string::npos);
}

TEST_F(ObservabilityRoutesTest, CorsPreflightOptionsUnlistedOriginNoAcao) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h{{"Origin", "https://evil.com"},
                       {"Access-Control-Request-Method", "GET"}};
    auto res = cli.Options("/api/v1/interactions", h);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_TRUE(res->get_header_value("Access-Control-Allow-Origin").empty());
}

}  // namespace
}  // namespace cortrix
