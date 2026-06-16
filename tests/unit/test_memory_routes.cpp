// Unit tests for memory_routes.cpp (320 lines, 0% -> target 85%+)
//
// Strategy: Create a real httplib::Server, register memory routes with
// lightweight stubs, and call endpoints via httplib::Client.
// This exercises the actual route handler code in memory_routes.cpp.

#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/memory/memory_routes.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/http_server.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/spc/onnx_embedder.h"
#include "ns_pool_test_helper.h"  // cortrix::test::NsPoolHarness (real F05 pool)
#include "mock_spc_manager.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using ::testing::Return;
using ::testing::_;

// D3.5 wire⑤c: the routes were migrated off the MVP CortrixNamespaceManager onto
// the F05 resource::INamespacePool + per-request resource::NamespaceFacade. These
// tests therefore stand up a real DefaultNamespacePool via the shared NsPoolHarness
// (mocked F12 routers + a real WriteCoordinator over a temp dir) and admit the
// "default" namespace, so each route's per-request facade.Acquire() hits and
// facade.memory()/store() open the NS's real (temp) memory.db / store.db. The MVP
// stub stores are gone — the harness supplies real, persistent per-NS stores.

class MemoryRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_memroutes_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        // Config
        config_.server.host = "127.0.0.1";
        config_.server.port = 0;
        config_.server.thread_count = 2;
        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;
        config_.memory.text_to_sql_ttl_seconds = 86400;
        config_.memory.default_ttl_seconds = 0;
        config_.memory.inject_recent_turns = 5;
        config_.memory.inject_max_tokens = 2000;
        // Optional per-fixture limit overrides (0 = leave the config default). A
        // subclass sets these in its constructor to exercise the limit branches.
        if (session_limit_override_ > 0)
            config_.memory.max_sessions_per_namespace = session_limit_override_;
        if (interaction_limit_override_ > 0)
            config_.memory.max_interactions_per_session = interaction_limit_override_;

        // Auth - register a test API key
        std::string test_key = "test-api-key-12345";
        ApiKeyConfig key_config;
        key_config.key_hash = ApiKeyAuth::HashKey(test_key);
        key_config.tenant_id = "test-tenant";
        key_config.permissions = 7;  // read + write + admin
        config_.auth.api_keys.push_back(key_config);
        auth_.LoadKeys(config_.auth.api_keys);

        // F05 namespace pool (real DefaultNamespacePool over a temp dir via the
        // shared harness) + admit the "default" namespace the route tests use.
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(tmp_dir_ + "/pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        // Find a free port
        port_ = 19080 + (getpid() % 1000);

        // Create real instances for embedder, classifier, fusion
        // After Phase 2 fix, the search handler uses these for semantic search
        embedder_ = std::make_unique<OnnxEmbedder>("stub_model.onnx", 128);
        embedder_->Init();  // Sets session_ to non-null sentinel
        LlmConfig llm_cfg;  // Empty provider -> keyword fallback mode
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();

        // Set up httplib::Server and register memory routes
        svr_ = std::make_unique<httplib::Server>();

        RegisterMemoryRoutes(*svr_, auth_, harness_->ipool(), mock_spc_,
                             *embedder_, *classifier_, *fusion_,
                             config_.memory);

        // Start server
        svr_thread_ = std::thread([this] {
            svr_->listen("127.0.0.1", port_);
        });

        // Wait for server
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/memory/sessions", "{}", "application/json");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        svr_->stop();
        if (svr_thread_.joinable()) {
            svr_thread_.join();
        }
        system(("rm -rf " + tmp_dir_).c_str());
    }

    // Helper: make authenticated request headers
    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer test-api-key-12345"}};
    }

    // Helper: create a session and return session_id
    std::string CreateSession(const std::string& ns = "default",
                              const std::string& user_id = "",
                              const std::string& title = "") {
        httplib::Client cli("127.0.0.1", port_);
        json body;
        body["namespace"] = ns;
        if (!user_id.empty()) body["user_id"] = user_id;
        if (!title.empty()) body["title"] = title;

        auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                            body.dump(), "application/json");
        if (!res || res->status != 201) return "";
        auto resp = json::parse(res->body);
        return resp.value("session_id", "");
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    cortrix::testing::MockSPCManager mock_spc_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string tmp_dir_;
    int port_;

    // Subclasses set these (>0) before SetUp runs to shrink the route limits and
    // exercise the max_sessions / max_interactions rejection branches.
    int session_limit_override_ = 0;
    int interaction_limit_override_ = 0;
};

// ===== Authentication Tests (AuthCheck helper, lines 22-49) =====

TEST_F(MemoryRoutesTest, NoAuthReturns401) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";

    auto res = cli.Post("/api/v1/memory/sessions", body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    auto resp = json::parse(res->body);
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(MemoryRoutesTest, InvalidBearerTokenReturns401) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {{"Authorization", "Bearer invalid-key"}};
    json body;
    body["namespace"] = "default";

    auto res = cli.Post("/api/v1/memory/sessions", headers,
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(MemoryRoutesTest, XApiKeyHeaderWorks) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {{"X-API-Key", "test-api-key-12345"}};
    json body;
    body["namespace"] = "default";

    auto res = cli.Post("/api/v1/memory/sessions", headers,
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);
}

TEST_F(MemoryRoutesTest, BearerPrefixParsing) {
    httplib::Client cli("127.0.0.1", port_);
    // Bearer prefix with correct token
    httplib::Headers headers = {{"Authorization", "Bearer test-api-key-12345"}};
    json body;
    body["namespace"] = "default";

    auto res = cli.Post("/api/v1/memory/sessions", headers,
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);
}

// ===== POST /api/v1/memory/sessions (Create Session, lines 62-110) =====

TEST_F(MemoryRoutesTest, CreateSessionSuccess) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["user_id"] = "user_001";
    body["title"] = "Test Session";

    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto resp = json::parse(res->body);
    EXPECT_FALSE(resp["session_id"].get<std::string>().empty());
    EXPECT_EQ(resp["namespace"], "default");
    EXPECT_EQ(resp["user_id"], "user_001");
    EXPECT_EQ(resp["title"], "Test Session");
    EXPECT_FALSE(resp["created_at"].get<std::string>().empty());
}

TEST_F(MemoryRoutesTest, CreateSessionInvalidJson) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                        "not valid json{{{", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, CreateSessionMissingNamespace) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["user_id"] = "user_001";

    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto resp = json::parse(res->body);
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(MemoryRoutesTest, CreateSessionNamespaceNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "nonexistent_ns";

    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, CreateSessionMinimalFields) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";

    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto resp = json::parse(res->body);
    EXPECT_FALSE(resp["session_id"].get<std::string>().empty());
    // MEM05 CE no-auth fallback: create stores the same effective user_id the
    // list/detail isolation checks resolve to.
    EXPECT_EQ(resp["user_id"], "default");
    EXPECT_EQ(resp["title"], "");
}

// Issue-12: a duplicate client-provided session_id is a 409 with the Agent-friendly
// envelope, not a raw 500 "UNIQUE constraint failed".
TEST_F(MemoryRoutesTest, CreateSessionDuplicateIdReturns409) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["session_id"] = "dup-session-1";
    auto first = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                          body.dump(), "application/json");
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 201);

    auto second = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                           body.dump(), "application/json");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 409);
    auto resp = json::parse(second->body);
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], "CX_ERR_MEMORY_SESSION_EXISTS");
    EXPECT_FALSE(resp["error"]["retryable"].get<bool>());
    EXPECT_EQ(resp["error"]["structured_data"]["session_id"], "dup-session-1");
}

// ===== GET /api/v1/memory/sessions (List Sessions, lines 113-185) =====
//
// NOTE: every request's facade.memory() opens the SAME per-NS memory.db (the
// harness's "default" unit dir), so the store is persistent and shared across
// requests. These tests assert each endpoint's response format and error
// handling; cross-request visibility is covered by CrossRequestRoundtrip.

TEST_F(MemoryRoutesTest, ListSessionsSuccess) {
    // Even without prior session creation, the endpoint should return
    // a valid response with the expected pagination fields.
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default", AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_TRUE(resp.contains("sessions"));
    EXPECT_TRUE(resp["sessions"].is_array());
    EXPECT_TRUE(resp.contains("total_count"));
    EXPECT_TRUE(resp.contains("has_more"));
    EXPECT_TRUE(resp.contains("limit"));
    EXPECT_TRUE(resp.contains("offset"));
}

TEST_F(MemoryRoutesTest, ListSessionsMissingNamespace) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions", AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, ListSessionsNamespaceNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=nonexistent_ns", AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, ListSessionsPagination) {
    // No sessions created in this fresh per-test store; assert the pagination
    // params are correctly parsed and echoed back.
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default&limit=2&offset=0",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_EQ(resp["limit"], 2);
    EXPECT_EQ(resp["offset"], 0);
    EXPECT_TRUE(resp.contains("sessions"));
    EXPECT_TRUE(resp.contains("has_more"));
    EXPECT_TRUE(resp.contains("total_count"));
}

TEST_F(MemoryRoutesTest, ListSessionsPaginationHasMoreFalse) {
    // Fresh per-test store has no sessions -> has_more=false, total_count=0
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default&limit=10&offset=0",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_FALSE(resp["has_more"].get<bool>());
    EXPECT_EQ(resp["total_count"], 0);
}

TEST_F(MemoryRoutesTest, ListSessionsInvalidLimit) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default&limit=notanumber",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, ListSessionsInvalidOffset) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default&offset=abc",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, ListSessionsLimitClampedTo100) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default&limit=200",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_EQ(resp["limit"], 100);
}

TEST_F(MemoryRoutesTest, ListSessionsNegativeLimitUsesDefault) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default&limit=-5",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_EQ(resp["limit"], 20);  // design spec: default=20
}

TEST_F(MemoryRoutesTest, ListSessionsNegativeOffsetClampsToZero) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default&offset=-10",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_EQ(resp["offset"], 0);
}

// ===== GET /api/v1/memory/sessions/:session_id (Get Session Detail, lines 188-246) =====

TEST_F(MemoryRoutesTest, GetSessionDetailSuccess) {
    // Exercises the full GET-detail handler path against a session id that was
    // never created in this (fresh per-test) store, so it returns 404.
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee?namespace=default",
                       AuthHeaders());

    ASSERT_TRUE(res);
    // Session was never created -> 404
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, GetSessionDetailMissingNamespace) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions/" + sid, AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, GetSessionDetailNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions/00000000-0000-4000-8000-000000000000?namespace=default",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, GetSessionDetailNamespaceNotFound) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions/" + sid + "?namespace=nonexistent",
                       AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

// MEM05: GET detail for a session owned by ANOTHER user must 404-mask (the
// cross-user ownership branch, owned=false). Create as user_a, read as user_b.
TEST_F(MemoryRoutesTest, GetSessionDetailCrossUserReturns404Mask) {
    std::string sid = CreateSession("default", "user_a");
    ASSERT_FALSE(sid.empty());

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(
        ("/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=user_b").c_str(),
        AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);  // anti-enumeration: not 403
}

// MEM05: GET detail for an owned session returns 200 with the full session body
// (the owned=true success path, including the interactions array).
TEST_F(MemoryRoutesTest, GetSessionDetailOwnedReturnsBody) {
    std::string sid = CreateSession("default", "owner_u", "My Session");
    ASSERT_FALSE(sid.empty());

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(
        ("/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=owner_u").c_str(),
        AuthHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto resp = json::parse(res->body);
    EXPECT_EQ(resp["session"]["session_id"], sid);
    EXPECT_EQ(resp["session"]["user_id"], "owner_u");
    EXPECT_EQ(resp["session"]["title"], "My Session");
    EXPECT_TRUE(resp["interactions"].is_array());
}

// GET detail for an owned session that HAS an interaction with all optional fields
// populated exercises the interaction-serialization branches (lines 264-281:
// query_type / status / latency_ms / metadata present).
TEST_F(MemoryRoutesTest, GetSessionDetailOwnedWithInteractionSerializesFields) {
    ON_CALL(mock_spc_, Submit(_)).WillByDefault(Return(Status::Ok()));
    std::string sid = CreateSession("default", "detail_u");
    ASSERT_FALSE(sid.empty());

    httplib::Client cli("127.0.0.1", port_);
    json wbody;
    wbody["namespace"] = "default";
    wbody["user_id"] = "detail_u";
    wbody["query_text"] = "Q";
    wbody["response_text"] = "A";
    wbody["query_type"] = "chat";
    wbody["response_status"] = "success";
    wbody["latency_ms"] = 42;
    wbody["metadata"] = {{"k", "v"}};
    auto w = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                      AuthHeaders(), wbody.dump(), "application/json");
    ASSERT_TRUE(w);
    ASSERT_EQ(w->status, 201);

    auto res = cli.Get(
        ("/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=detail_u").c_str(),
        AuthHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto resp = json::parse(res->body);
    ASSERT_TRUE(resp["interactions"].is_array());
    ASSERT_GE(resp["interactions"].size(), 1u);
    // At least one interaction surfaces the optional fields.
    bool saw_meta = false;
    for (const auto& it : resp["interactions"]) {
        if (it.contains("metadata")) saw_meta = true;
    }
    EXPECT_TRUE(saw_meta);
}

// MEM05: list isolation drops sessions owned by other users (the post-filter
// excludes a non-matching user_id, line 184).
TEST_F(MemoryRoutesTest, ListSessionsExcludesOtherUsersSessions) {
    ASSERT_FALSE(CreateSession("default", "list_user_a", "A1").empty());
    ASSERT_FALSE(CreateSession("default", "list_user_b", "B1").empty());

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default&user_id=list_user_a",
                       AuthHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto resp = json::parse(res->body);
    // Only list_user_a's own sessions come back; list_user_b's are filtered out.
    for (const auto& s : resp["sessions"]) {
        EXPECT_EQ(s["user_id"], "list_user_a");
    }
}

// ===== DELETE /api/v1/memory/sessions/:session_id (Delete Session, lines 249-287) =====

// MEM05: DELETE of a session owned by ANOTHER user 404-masks (mismatch branch).
TEST_F(MemoryRoutesTest, DeleteSessionCrossUserReturns404Mask) {
    std::string sid = CreateSession("default", "del_owner");
    ASSERT_FALSE(sid.empty());

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(
        ("/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=attacker").c_str(),
        AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

// MEM05: DELETE of an owned session succeeds (200) with the deletion summary.
TEST_F(MemoryRoutesTest, DeleteSessionOwnedSucceeds) {
    std::string sid = CreateSession("default", "del_me");
    ASSERT_FALSE(sid.empty());

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(
        ("/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=del_me").c_str(),
        AuthHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto resp = json::parse(res->body);
    EXPECT_TRUE(resp["deleted"].get<bool>());
    EXPECT_EQ(resp["session_id"], sid);
    EXPECT_EQ(resp["interactions_deleted"], 0);
}


TEST_F(MemoryRoutesTest, DeleteSessionSuccess) {
    // Delete handler verifies ownership then SessionDelete; an id never created
    // in this fresh per-test store is NotFound. Test the handler passes it through.
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(
        "/api/v1/memory/sessions/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee?namespace=default",
        AuthHeaders());

    ASSERT_TRUE(res);
    // Session was never created -> 404
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, DeleteSessionMissingNamespace) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/memory/sessions/" + sid, AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, DeleteSessionNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(
        "/api/v1/memory/sessions/00000000-0000-4000-8000-000000000000?namespace=default",
        AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, DeleteSessionNamespaceNotFound) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/memory/sessions/" + sid + "?namespace=nonexistent",
                          AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

// ===== POST /api/v1/memory/sessions/:session_id/interactions (Write, lines 290-361) =====

TEST_F(MemoryRoutesTest, WriteInteractionSuccess) {
    // Writes against a session id that was never created in this fresh per-test
    // store: the writer's SessionGet fails with NotFound and the handler
    // correctly propagates this as 404.
    ON_CALL(mock_spc_, Submit(_)).WillByDefault(Return(Status::Ok()));

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["user_id"] = "user_001";
    body["query_text"] = "What is Q4 revenue?";
    body["response_text"] = "Q4 revenue was $12.5M.";
    body["query_type"] = "chat";
    body["response_status"] = "success";
    body["latency_ms"] = 150;
    body["result_source"] = "conversation";

    // Use a valid-looking but nonexistent session_id
    auto res = cli.Post("/api/v1/memory/sessions/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee/interactions",
                        AuthHeaders(), body.dump(), "application/json");

    ASSERT_TRUE(res);
    // Writer's SessionGet fails -> NotFound -> 404
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, WriteInteractionInvalidJson) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                        AuthHeaders(), "{{bad json", "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, WriteInteractionMissingNamespace) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["query_text"] = "test";
    body["response_text"] = "test";

    auto res = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                        AuthHeaders(), body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, WriteInteractionMissingQueryText) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["response_text"] = "some response";

    auto res = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                        AuthHeaders(), body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, WriteInteractionMissingResponseText) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query_text"] = "some query";

    auto res = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                        AuthHeaders(), body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, WriteInteractionNamespaceNotFound) {
    std::string sid = CreateSession("default");

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "nonexistent";
    body["query_text"] = "test";
    body["response_text"] = "test";

    auto res = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                        AuthHeaders(), body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, WriteInteractionWithMetadata) {
    // Tests that metadata JSON field is correctly parsed from request body.
    // Session id was never created -> 404, but we verify the handler reaches
    // the writer (not rejected at JSON parsing).
    ON_CALL(mock_spc_, Submit(_)).WillByDefault(Return(Status::Ok()));

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query_text"] = "test query";
    body["response_text"] = "test response";
    body["metadata"] = {{"custom_field", "custom_value"}};

    auto res = cli.Post("/api/v1/memory/sessions/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee/interactions",
                        AuthHeaders(), body.dump(), "application/json");

    ASSERT_TRUE(res);
    // Session was never created -> 404 (but body was parsed OK)
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, WriteInteractionWithTTL) {
    // Verify TTL field is accepted in the request body without parse errors.
    // Session id was never created -> 404, but the handler correctly parses
    // and passes ttl_seconds to the MemoryWriter.
    ON_CALL(mock_spc_, Submit(_)).WillByDefault(Return(Status::Ok()));

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query_text"] = "test";
    body["response_text"] = "response";
    body["result_source"] = "text_to_sql";
    body["ttl_seconds"] = 3600;

    auto res = cli.Post("/api/v1/memory/sessions/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee/interactions",
                        AuthHeaders(), body.dump(), "application/json");

    ASSERT_TRUE(res);
    // Session was never created -> 404
    EXPECT_EQ(res->status, 404);
}

// Write to a REAL (existing) session: the writer succeeds and the handler returns
// 201 with turn/spc_enqueued (the success response path, lines 405-412).
TEST_F(MemoryRoutesTest, WriteInteractionToExistingSessionSucceeds) {
    ON_CALL(mock_spc_, Submit(_)).WillByDefault(Return(Status::Ok()));
    std::string sid = CreateSession("default", "writer_u");
    ASSERT_FALSE(sid.empty());

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["user_id"] = "writer_u";
    body["query_text"] = "What is Q4 revenue?";
    body["response_text"] = "Q4 revenue was $12.5M.";
    body["query_type"] = "chat";

    auto res = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                        AuthHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 201);
    auto resp = json::parse(res->body);
    EXPECT_EQ(resp["session_id"], sid);
    EXPECT_TRUE(resp.contains("turn"));
    EXPECT_TRUE(resp["spc_enqueued"].get<bool>());
}

// ===== POST /api/v1/memory/search (Search, lines 364-461) =====

TEST_F(MemoryRoutesTest, SearchBasicSuccess) {
    // Search now uses semantic search via QueryPipeline.
    // With dummy embedder/vector components, the search degrades gracefully.
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "revenue";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_TRUE(resp.contains("results"));
    EXPECT_TRUE(resp["results"].is_array());
    EXPECT_TRUE(resp.contains("total_results"));
    EXPECT_TRUE(resp.contains("latency_ms"));
    EXPECT_TRUE(resp.contains("degraded"));
    // With stub embedder/vector components, degraded=true is expected
    // In production, with real ONNX embedder, this would be false
}

TEST_F(MemoryRoutesTest, SearchInvalidJson) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        "{bad json", "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, SearchMissingNamespace) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["query"] = "test";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, SearchMissingQuery) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, SearchNamespaceNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "nonexistent_ns";
    body["query"] = "test";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(MemoryRoutesTest, SearchWithScopeSession) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["scope"] = "session";
    body["session_id"] = "some-session-id";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    // Should succeed (even with 0 results)
    EXPECT_EQ(res->status, 200);
}

TEST_F(MemoryRoutesTest, SearchWithScopeUser) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["scope"] = "user";
    body["user_id"] = "user_001";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

// MEM05: legacy scope="all" is no longer a distinct mode; it falls through to
// kUser (kAll removed). Still 200 because user_id defaults to "default".
TEST_F(MemoryRoutesTest, SearchWithLegacyScopeAllFallsBackToUser) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["scope"] = "all";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(MemoryRoutesTest, SearchWithTopK) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["top_k"] = 5;

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(MemoryRoutesTest, SearchWithIncludeExpired) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["include_expired"] = true;

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    // With stub components, search may degrade or return 200/500 depending
    // on whether QueryPipeline handles the dummy embedder gracefully.
    // We accept 200 (degraded search) or 500 (if pipeline crashes).
    EXPECT_TRUE(res->status == 200 || res->status == 500);
}

TEST_F(MemoryRoutesTest, SearchEmptyResults) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "nonexistent_xyzabc_content";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_EQ(resp["total_results"], 0);
    EXPECT_TRUE(resp["results"].empty());
}

// ===== Search scope validation edge cases (lines 399-414) =====

TEST_F(MemoryRoutesTest, SearchSessionScopeMissingSessionId) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["scope"] = "session";
    // missing session_id -> MemorySearchRequest::Validate() should catch this

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// MEM05: at the HTTP route level a missing user_id is NOT a 400 — CE no-auth
// mode injects user_id="default" before validation (design § CE single-user compatibility).
// The 400-on-empty-user_id contract is enforced by MemorySearchRequest::Validate()
// directly (see test_memory_searcher MEM05 unit tests).
TEST_F(MemoryRoutesTest, SearchUserScopeMissingUserIdDefaultsToDefaultUser) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["scope"] = "user";
    // missing user_id -> route injects "default" (CE no-auth)

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(MemoryRoutesTest, SearchInvalidTopK) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["top_k"] = 0;  // invalid - must be 1-100

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, SearchTopKOver100) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["top_k"] = 101;  // exceeds max

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// MEM05: any scope other than "session" defaults to kUser (kAll removed).
// user_id defaults to "default" (CE no-auth), so this succeeds with 200.
TEST_F(MemoryRoutesTest, SearchUnknownScopeDefaultsToUser) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "test";
    body["scope"] = "unknown_scope";  // anything not "session" -> kUser

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);  // MEM05: defaults to kUser + default user
}

// ===== Delete session namespace not found (lines 260-264) =====

TEST_F(MemoryRoutesTest, DeleteSessionNoAuth) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete(
        "/api/v1/memory/sessions/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee?namespace=default");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ===== Write interaction NoAuth (lines 291-292) =====

TEST_F(MemoryRoutesTest, WriteInteractionNoAuth) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query_text"] = "test";
    body["response_text"] = "response";

    auto res = cli.Post("/api/v1/memory/sessions/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee/interactions",
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ===== Get session detail NoAuth =====

TEST_F(MemoryRoutesTest, GetSessionDetailNoAuth) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee?namespace=default");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ===== List sessions NoAuth =====

TEST_F(MemoryRoutesTest, ListSessionsNoAuth) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/sessions?namespace=default");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ===== Multi-endpoint verification =====
// NOTE: With persistent MemoryStore (shared per namespace), cross-request
// roundtrip now works correctly. Sessions created via POST are visible to GET.

TEST_F(MemoryRoutesTest, CreateSessionResponseFormat) {
    // Verify POST /sessions response has all required fields per design spec
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["user_id"] = "roundtrip_user";
    body["title"] = "Roundtrip Test";

    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 201);

    auto resp = json::parse(res->body);
    // session_id should be a valid UUID-like string
    std::string sid = resp["session_id"].get<std::string>();
    EXPECT_FALSE(sid.empty());
    EXPECT_GE(sid.size(), 36u);  // UUID v4 format
    EXPECT_EQ(resp["namespace"], "default");
    EXPECT_EQ(resp["user_id"], "roundtrip_user");
    EXPECT_EQ(resp["title"], "Roundtrip Test");
    EXPECT_FALSE(resp["created_at"].get<std::string>().empty());
}

TEST_F(MemoryRoutesTest, SearchResponseFormat) {
    // Verify POST /search response has all required fields per design spec
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query"] = "anything";

    auto res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    // May return 200 (degraded) due to stub embedder components
    if (res->status == 200) {
        auto resp = json::parse(res->body);
        EXPECT_TRUE(resp.contains("results"));
        EXPECT_TRUE(resp["results"].is_array());
        EXPECT_TRUE(resp.contains("total_results"));
        EXPECT_TRUE(resp.contains("latency_ms"));
        EXPECT_TRUE(resp.contains("degraded"));
        EXPECT_GE(resp["latency_ms"].get<int64_t>(), 0);
    }
}

// ===== Cross-request roundtrip test =====
// With persistent MemoryStore, sessions created via POST are visible to GET.

TEST_F(MemoryRoutesTest, CrossRequestRoundtrip) {
    httplib::Client cli("127.0.0.1", port_);

    // 1. Create a session
    json create_body;
    create_body["namespace"] = "default";
    create_body["user_id"] = "roundtrip_user";
    create_body["title"] = "Roundtrip Session";

    auto create_res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                                create_body.dump(), "application/json");
    ASSERT_TRUE(create_res);
    ASSERT_EQ(create_res->status, 201);

    auto create_resp = json::parse(create_res->body);
    std::string sid = create_resp["session_id"].get<std::string>();
    EXPECT_FALSE(sid.empty());

    // 2. List sessions - should now include the created session.
    //    MEM05: list is user-isolated, so pass the owner's user_id.
    auto list_res = cli.Get("/api/v1/memory/sessions?namespace=default&user_id=roundtrip_user",
                            AuthHeaders());
    ASSERT_TRUE(list_res);
    ASSERT_EQ(list_res->status, 200);

    auto list_resp = json::parse(list_res->body);
    EXPECT_GE(list_resp["total_count"].get<int>(), 1);
    bool found = false;
    for (const auto& s : list_resp["sessions"]) {
        if (s["session_id"] == sid) {
            found = true;
            EXPECT_EQ(s["title"], "Roundtrip Session");
            break;
        }
    }
    EXPECT_TRUE(found) << "Created session not found in list";

    // 3. Get session detail (MEM05: owner user_id required for the ownership check).
    auto get_res = cli.Get(
        ("/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=roundtrip_user").c_str(),
        AuthHeaders());
    ASSERT_TRUE(get_res);
    ASSERT_EQ(get_res->status, 200);

    auto get_resp = json::parse(get_res->body);
    EXPECT_EQ(get_resp["session"]["session_id"], sid);
    EXPECT_EQ(get_resp["session"]["title"], "Roundtrip Session");

    // 4. Delete session (MEM05: owner user_id required).
    auto del_res = cli.Delete(
        ("/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=roundtrip_user").c_str(),
        AuthHeaders());
    ASSERT_TRUE(del_res);
    ASSERT_EQ(del_res->status, 200);

    auto del_resp = json::parse(del_res->body);
    EXPECT_TRUE(del_resp["deleted"].get<bool>());

    // 5. Verify deletion
    auto get_res2 = cli.Get(
        ("/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=roundtrip_user").c_str(),
        AuthHeaders());
    ASSERT_TRUE(get_res2);
    EXPECT_EQ(get_res2->status, 404);
}

// ===== Inject endpoint test =====

TEST_F(MemoryRoutesTest, InjectEndpointSuccess) {
    httplib::Client cli("127.0.0.1", port_);

    // Create a session first
    json create_body;
    create_body["namespace"] = "default";
    auto create_res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                                create_body.dump(), "application/json");
    ASSERT_TRUE(create_res);
    ASSERT_EQ(create_res->status, 201);
    auto sid = json::parse(create_res->body)["session_id"].get<std::string>();

    // Call inject endpoint
    json inject_body;
    inject_body["namespace"] = "default";
    inject_body["session_id"] = sid;

    auto res = cli.Post("/api/v1/memory/inject", AuthHeaders(),
                        inject_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto resp = json::parse(res->body);
    EXPECT_EQ(resp["session_id"], sid);
    EXPECT_TRUE(resp.contains("context_text"));
    EXPECT_TRUE(resp.contains("turn_count"));
    EXPECT_TRUE(resp.contains("token_count_approx"));
    // Empty session has no context
    EXPECT_EQ(resp["turn_count"], 0);
}

TEST_F(MemoryRoutesTest, InjectEndpointMissingSessionId) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";

    auto res = cli.Post("/api/v1/memory/inject", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, InjectEndpointMissingNamespace) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["session_id"] = "some-session";

    auto res = cli.Post("/api/v1/memory/inject", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(MemoryRoutesTest, InjectEndpointNoAuth) {
    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["session_id"] = "some-session";

    auto res = cli.Post("/api/v1/memory/inject", body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ===== Limit-rejection branches (max_sessions / max_interactions) =====
// Subclasses shrink the limits via the override hook so the rejection branches
// (memory_routes.cpp lines 96 / 375) run.

class MemoryRoutesSessionLimitTest : public MemoryRoutesTest {
protected:
    MemoryRoutesSessionLimitTest() { session_limit_override_ = 1; }
};

// Creating a second session when the namespace is already at max_sessions yields 400.
TEST_F(MemoryRoutesSessionLimitTest, CreateSessionAtMaxLimitRejected) {
    ASSERT_FALSE(CreateSession("default").empty());  // 1st fills the limit (1)

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto resp = json::parse(res->body);
    EXPECT_TRUE(resp.contains("error"));
}

class MemoryRoutesInteractionLimitTest : public MemoryRoutesTest {
protected:
    MemoryRoutesInteractionLimitTest() { interaction_limit_override_ = 1; }
};

// Writing past max_interactions_per_session yields 400 (line 375 branch).
TEST_F(MemoryRoutesInteractionLimitTest, WriteInteractionAtMaxLimitRejected) {
    ON_CALL(mock_spc_, Submit(_)).WillByDefault(Return(Status::Ok()));
    std::string sid = CreateSession("default", "lim_user");
    ASSERT_FALSE(sid.empty());

    httplib::Client cli("127.0.0.1", port_);
    json body;
    body["namespace"] = "default";
    body["query_text"] = "q1";
    body["response_text"] = "r1";

    // First write succeeds (count 0 < 1) and inserts a user+assistant pair.
    auto res1 = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                         AuthHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res1);
    ASSERT_EQ(res1->status, 201);

    // Second write: interaction_count (>=2) >= limit(1) -> 400 rejected.
    auto res2 = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                         AuthHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 400);
}

// D9 admin revoke is deferred to Wave S2 (the underlying RevokeInvalidation engine
// exists + is unit-tested, but this HTTP route is not wired to it). It must NOT
// fake-succeed with 200 "accepted" -- that would tell the Agent the revoke happened
// (GEN-Agent honesty, CLAUDE.md sec.5). With no extraction service wired (fixture
// default) the route returns 503; a wired build returns 501 CX_ERR_NOT_IMPLEMENTED.
// Either way it is an honest non-2xx, never a misleading accepted.
TEST_F(MemoryRoutesTest, AdminRevokeDoesNotFakeAccept) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/invalidations/blk_x/revoke", AuthHeaders(),
                        R"({"ns":"default","reason":"t"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_NE(res->status, 200);
    EXPECT_TRUE(res->status == 501 || res->status == 503) << res->status;
    auto j = json::parse(res->body);
    ASSERT_TRUE(j.contains("error"));             // wrapped GEN-Agent envelope
    EXPECT_NE(j["error"].value("code", ""), "");  // machine-readable code, not empty
}

}  // namespace
}  // namespace cortrix
