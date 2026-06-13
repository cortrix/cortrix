/// @file test_e2e_full_server.cpp
/// @brief D4 Comprehensive E2E: Full HTTP server with ALL route groups
///
/// Tests complete user journeys through the full Cortrix HTTP API:
///   1. Complete user journey: NS → Upload → Query → Memory → Delete
///   2. Auth enforcement across all endpoints
///   3. Error response format consistency
///   4. Upload dedup via content hash
///   5. Multi-namespace data isolation
///   6. Concurrent HTTP operations
///   7. Namespace CRUD lifecycle
///   8. Query parameter validation
///   9. Memory session listing / pagination
///  10. Memory search endpoint
///  11. Request ID propagation
///  12. Upload different MIME types
///  13. Memory interaction with metadata
///  14. Large query truncation
///  15. Upload empty file
///  16. Unicode content handling
///  17. Multiple uploads then query
///  18. Session delete and re-create

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <vector>
#include <set>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/server/http_server.h"
#include "cortrix/server/routes/document_routes.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/memory/memory_routes.h"
#include "cortrix/upload/upload_handler.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/rrf_fusion.h"
#include "ns_pool_test_helper.h"  // [wire⑤c] F05 NsPoolHarness replaces MVP NamespaceManager

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ==================================================================
// Full Server E2E Fixture — httplib::Server with ALL route groups
// Uses same pattern as existing MemoryPersistenceHttpTest/FullHttpE2ETest
// ==================================================================

class FullServerE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_e2e_fsrv_" + std::to_string(getpid()) +
                    "_" + std::to_string(test_counter_++));
        fs::create_directories(tmp_dir_);

        config_.ns.data_dir = tmp_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.upload.max_file_size = 10 * 1024 * 1024;
        config_.upload.large_file_threshold = 5 * 1024 * 1024;
        config_.memory.inject_recent_turns = 3;
        config_.memory.inject_max_tokens = 2000;
        config_.memory.default_ttl_seconds = 0;
        config_.memory.max_sessions_per_namespace = 100;
        config_.memory.max_interactions_per_session = 500;

        // Auth keys: admin, read-write, read-only
        admin_key_ = "test-admin-key";
        rw_key_ = "test-rw-key";
        ro_key_ = "test-ro-key";

        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "e2e";
        admin_kc.permissions = kPermRead | kPermWrite | kPermAdmin;

        ApiKeyConfig rw_kc;
        rw_kc.key_hash = ApiKeyAuth::HashKey(rw_key_);
        rw_kc.tenant_id = "e2e";
        rw_kc.permissions = kPermRead | kPermWrite;

        ApiKeyConfig ro_kc;
        ro_kc.key_hash = ApiKeyAuth::HashKey(ro_key_);
        ro_kc.tenant_id = "e2e";
        ro_kc.permissions = kPermRead;

        auth_ = std::make_unique<ApiKeyAuth>();
        auth_->LoadKeys({admin_kc, rw_kc, ro_kc});

        // [wire⑤c] F05 NS resource pool replaces the MVP NamespaceManager +
        // CortrixNamespaceManager. All route groups now take INamespacePool&;
        // tests admit each namespace via harness_->Admit(ns) instead of Create.
        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);

        embedder_ = std::make_unique<OnnxEmbedder>("stub.onnx", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();

        spc_mgr_ = std::make_unique<StubSPCManager>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_mgr_);

        svr_ = std::make_unique<httplib::Server>();

        // Register ALL route groups
        RegisterDocumentRoutes(*svr_, *handler_, harness_->ipool(), *auth_);
        RegisterQueryRoutes(*svr_, *auth_, harness_->ipool(),
                            *embedder_, *classifier_, *fusion_);
        RegisterMemoryRoutes(*svr_, *auth_, harness_->ipool(), *spc_mgr_,
                             *embedder_, *classifier_, *fusion_,
                             config_.memory);

        port_ = svr_->bind_to_any_port("127.0.0.1");
        svr_thread_ = std::thread([this] {
            svr_->listen_after_bind();
        });

        // Wait for server
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/memory/sessions");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    httplib::Headers Admin() {
        return {{"Authorization", "Bearer " + admin_key_}};
    }
    httplib::Headers RW() {
        return {{"Authorization", "Bearer " + rw_key_}};
    }
    httplib::Headers RO() {
        return {{"Authorization", "Bearer " + ro_key_}};
    }

    // Stub SPCManager
    class StubSPCManager : public SPCManager {
    public:
        StubSPCManager() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    CortrixConfig config_;
    fs::path tmp_dir_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<StubSPCManager> spc_mgr_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string admin_key_, rw_key_, ro_key_;
    int port_ = 0;
    static int test_counter_;
};

int FullServerE2ETest::test_counter_ = 0;

// ------------------------------------------------------------------
// Test 1: Complete User Journey
// Create NS → Upload Doc → Status → Query → Memory → Inject → Delete
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, CompleteUserJourney) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("user_proj").ok());

    // Upload document
    httplib::MultipartFormDataItems items = {
        {"file", "Machine learning uses neural networks for pattern recognition. "
                 "Deep learning is a subset of machine learning.",
         "ml_intro.txt", "text/plain"},
    };
    auto upload = cli.Post("/api/v1/namespaces/user_proj/documents", Admin(), items);
    ASSERT_TRUE(upload);
    ASSERT_EQ(upload->status, 201) << upload->body;
    auto upbody = json::parse(upload->body);
    std::string doc_id = upbody["doc_id"].get<std::string>();  // [D-I6] doc_id is a ULID string now
    EXPECT_FALSE(doc_id.empty());
    EXPECT_EQ(upbody["status"], "pending");

    // Check document status
    auto status = cli.Get("/api/v1/namespaces/user_proj/documents/" +
                          doc_id + "/status", Admin());
    ASSERT_TRUE(status);
    EXPECT_EQ(status->status, 200);

    // Query (doc pending → empty/degraded result expected)
    json qbody;
    qbody["query"] = "machine learning";
    qbody["namespace"] = "user_proj";
    qbody["top_k"] = 5;
    auto query = cli.Post("/api/v1/query", Admin(), qbody.dump(), "application/json");
    ASSERT_TRUE(query);
    EXPECT_TRUE(query->status == 200 || query->status == 503);

    // Create memory session
    json sess;
    sess["namespace"] = "user_proj";
    sess["user_id"] = "test_user";
    sess["title"] = "E2E Journey";
    auto sr = cli.Post("/api/v1/memory/sessions", Admin(),
                       sess.dump(), "application/json");
    ASSERT_TRUE(sr && sr->status == 201);
    std::string sid = json::parse(sr->body)["session_id"].get<std::string>();

    // Write interaction
    json inter;
    inter["namespace"] = "user_proj";
    inter["query_text"] = "What is machine learning?";
    inter["response_text"] = "ML is a subset of AI.";
    auto wr = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                       Admin(), inter.dump(), "application/json");
    ASSERT_TRUE(wr && wr->status == 201);

    // Memory inject
    json inj;
    inj["namespace"] = "user_proj";
    inj["session_id"] = sid;
    auto inject = cli.Post("/api/v1/memory/inject", Admin(),
                           inj.dump(), "application/json");
    ASSERT_TRUE(inject && inject->status == 200);
    auto ijson = json::parse(inject->body);
    EXPECT_GE(ijson["turn_count"].get<int>(), 1);

    // Get session detail
    auto detail = cli.Get("/api/v1/memory/sessions/" + sid +
                          "?namespace=user_proj&user_id=test_user", Admin());
    ASSERT_TRUE(detail && detail->status == 200);
    auto djson = json::parse(detail->body);
    EXPECT_EQ(djson["session"]["user_id"], "test_user");
    EXPECT_GE(djson["interactions"].size(), 2u);

    // Delete session
    auto del = cli.Delete("/api/v1/memory/sessions/" + sid +
                          "?namespace=user_proj&user_id=test_user", Admin());
    ASSERT_TRUE(del && del->status == 200);

    // Verify session deleted
    auto gone = cli.Get("/api/v1/memory/sessions/" + sid +
                        "?namespace=user_proj", Admin());
    ASSERT_TRUE(gone);
    EXPECT_NE(gone->status, 200);
}

// ------------------------------------------------------------------
// Test 2: Auth enforcement across all endpoints
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, AuthEnforcementAllEndpoints) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("auth_test").ok());

    // No auth → 401 for protected endpoints
    auto r1 = cli.Post("/api/v1/query",
                       json({{"query","test"},{"namespace","auth_test"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->status, 401);

    auto r2 = cli.Post("/api/v1/memory/sessions",
                       json({{"namespace","auth_test"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 401);

    // Invalid key → 401
    httplib::Headers bad = {{"Authorization", "Bearer invalid-key"}};
    auto r3 = cli.Post("/api/v1/query", bad,
                       json({{"query","test"},{"namespace","auth_test"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 401);

    // Read-only key → 403 for write operations
    httplib::MultipartFormDataItems items = {
        {"file", "test", "test.txt", "text/plain"},
    };
    auto r4 = cli.Post("/api/v1/namespaces/auth_test/documents", RO(), items);
    ASSERT_TRUE(r4);
    EXPECT_EQ(r4->status, 403);

    // Read-only key → can query (read op)
    auto r5 = cli.Post("/api/v1/query", RO(),
                       json({{"query","test"},{"namespace","auth_test"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r5);
    EXPECT_NE(r5->status, 403) << "Read-only should be able to query";
}

// ------------------------------------------------------------------
// Test 3: Error response format consistency
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, ErrorFormatConsistency) {
    httplib::Client cli("127.0.0.1", port_);

    // 400: Invalid JSON to query
    auto r1 = cli.Post("/api/v1/query", Admin(),
                       "{bad json}", "application/json");
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->status, 400);
    auto j1 = json::parse(r1->body);
    EXPECT_TRUE(j1.contains("error"));
    EXPECT_TRUE(j1["error"].contains("code"));
    EXPECT_TRUE(j1["error"].contains("message"));
    EXPECT_TRUE(j1["error"].contains("timestamp"));

    // 400: Missing required field
    auto r2 = cli.Post("/api/v1/query", Admin(),
                       json({{"namespace","default"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 400);
    auto j2 = json::parse(r2->body);
    EXPECT_TRUE(j2.contains("error"));

    // 401: No auth
    auto r3 = cli.Post("/api/v1/memory/sessions",
                       json({{"namespace","x"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 401);
    auto j3 = json::parse(r3->body);
    EXPECT_TRUE(j3.contains("error"));
}

// ------------------------------------------------------------------
// Test 4: Upload dedup via content hash
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, UploadDedupByContentHash) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("dedup_test").ok());

    std::string content = "Identical content for dedup testing.";

    // First upload
    httplib::MultipartFormDataItems items1 = {
        {"file", content, "doc.txt", "text/plain"},
    };
    auto r1 = cli.Post("/api/v1/namespaces/dedup_test/documents", Admin(), items1);
    ASSERT_TRUE(r1 && r1->status == 201);
    auto b1 = json::parse(r1->body);
    std::string doc_id1 = b1["doc_id"].get<std::string>();  // [D-I6] ULID string

    // Same content → dedup
    httplib::MultipartFormDataItems items2 = {
        {"file", content, "doc.txt", "text/plain"},
    };
    auto r2 = cli.Post("/api/v1/namespaces/dedup_test/documents", Admin(), items2);
    ASSERT_TRUE(r2 != nullptr) << "Dedup upload returned null";
    EXPECT_EQ(r2->status, 200) << "Dedup should return 200, body: " << r2->body;
    auto b2 = json::parse(r2->body);
    EXPECT_EQ(b2["status"], "skipped");
    EXPECT_EQ(b2["doc_id"].get<std::string>(), doc_id1);

    // Different content → new doc
    httplib::MultipartFormDataItems items3 = {
        {"file", "Completely different content.", "doc2.txt", "text/plain"},
    };
    auto r3 = cli.Post("/api/v1/namespaces/dedup_test/documents", Admin(), items3);
    ASSERT_TRUE(r3 && r3->status == 201);
    auto b3 = json::parse(r3->body);
    EXPECT_NE(b3["doc_id"].get<std::string>(), doc_id1);
}

// ------------------------------------------------------------------
// Test 5: Multi-namespace isolation
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, MultiNamespaceIsolation) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("proj_alpha").ok());
    ASSERT_TRUE(harness_->Admit("proj_beta").ok());

    // Upload to alpha
    httplib::MultipartFormDataItems items = {
        {"file", "Alpha confidential data.", "alpha.txt", "text/plain"},
    };
    auto u = cli.Post("/api/v1/namespaces/proj_alpha/documents", Admin(), items);
    ASSERT_TRUE(u && u->status == 201);

    // Create memory in alpha
    json sa;
    sa["namespace"] = "proj_alpha";
    sa["user_id"] = "alice";
    auto r1 = cli.Post("/api/v1/memory/sessions", Admin(),
                       sa.dump(), "application/json");
    ASSERT_TRUE(r1 && r1->status == 201);
    std::string sid_a = json::parse(r1->body)["session_id"].get<std::string>();

    // Create memory in beta
    json sb;
    sb["namespace"] = "proj_beta";
    sb["user_id"] = "bob";
    auto r2 = cli.Post("/api/v1/memory/sessions", Admin(),
                       sb.dump(), "application/json");
    ASSERT_TRUE(r2 && r2->status == 201);

    // List sessions in beta → should NOT see alpha's session
    auto list_b = cli.Get("/api/v1/memory/sessions?namespace=proj_beta&user_id=bob", Admin());
    ASSERT_TRUE(list_b && list_b->status == 200);
    auto lb = json::parse(list_b->body);
    for (const auto& s : lb["sessions"]) {
        EXPECT_NE(s["session_id"], sid_a) << "Alpha leaked to beta";
    }

    // List sessions in alpha → should see alpha's session
    auto list_a = cli.Get("/api/v1/memory/sessions?namespace=proj_alpha&user_id=alice", Admin());
    ASSERT_TRUE(list_a && list_a->status == 200)
        << "List alpha status=" << (list_a ? list_a->status : -1)
        << " body=" << (list_a ? list_a->body : "null");
    auto la = json::parse(list_a->body);
    bool found = false;
    for (const auto& s : la["sessions"]) {
        if (s["session_id"].get<std::string>() == sid_a) found = true;
    }
    EXPECT_TRUE(found)
        << "Alpha session " << sid_a << " not found in list. "
        << "Response: " << la.dump(2);
}

// ------------------------------------------------------------------
// Test 6: Concurrent HTTP operations
// ------------------------------------------------------------------
// Re-enabled 2026-06-10 (D-I1.bis): this used to segfault inside
// sqlite3DbMallocRawNN — the per-namespace store connection was shared across
// request threads via per-request façade views (a single SQLite connection is
// not thread-safe across threads). Fixed by giving every façade its own
// private WAL store.db connection (thread isolation via object lifetime);
// writer contention queues on busy_timeout. This test is the regression guard
// for that fix: 6 concurrent uploads + memory sessions on ONE namespace.
TEST_F(FullServerE2ETest, ConcurrentHttpOperations) {
    ASSERT_TRUE(harness_->Admit("conc_test").ok());

    const int N = 6;
    std::atomic<int> upload_ok{0}, session_ok{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&, t]() {
            httplib::Client c("127.0.0.1", port_);
            c.set_read_timeout(10, 0);

            // Upload
            httplib::MultipartFormDataItems items = {
                {"file", "Thread " + std::to_string(t) + " content.",
                 "t" + std::to_string(t) + ".txt", "text/plain"},
            };
            auto u = c.Post("/api/v1/namespaces/conc_test/documents",
                           Admin(), items);
            if (u && u->status == 201) upload_ok.fetch_add(1);

            // Memory session
            json sb;
            sb["namespace"] = "conc_test";
            sb["user_id"] = "user_" + std::to_string(t);
            auto s = c.Post("/api/v1/memory/sessions", Admin(),
                           sb.dump(), "application/json");
            if (s && s->status == 201) session_ok.fetch_add(1);
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(upload_ok.load(), N);
    EXPECT_EQ(session_ok.load(), N);
}

// ------------------------------------------------------------------
// Test 7: Query parameter validation
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, QueryParameterValidation) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("qval_test").ok());

    // Empty query → 400
    auto r1 = cli.Post("/api/v1/query", Admin(),
                       json({{"query",""},{"namespace","qval_test"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->status, 400);

    // Missing namespace → 400
    auto r2 = cli.Post("/api/v1/query", Admin(),
                       json({{"query","test"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 400);

    // Nonexistent namespace → 404
    auto r3 = cli.Post("/api/v1/query", Admin(),
                       json({{"query","test"},{"namespace","no_such_ns"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 404);

    // Valid query on empty namespace → 200 or 503
    auto r4 = cli.Post("/api/v1/query", Admin(),
                       json({{"query","hello"},{"namespace","qval_test"},
                             {"top_k",5}}).dump(),
                       "application/json");
    ASSERT_TRUE(r4);
    EXPECT_TRUE(r4->status == 200 || r4->status == 503);
}

// ------------------------------------------------------------------
// Test 8: Memory session listing with total count
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, MemorySessionListing) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("list_test").ok());

    // Create 4 sessions, all owned by the same user (MEM05: list is
    // user-isolated, so cross-user listing in one call is not possible).
    for (int i = 0; i < 4; ++i) {
        json body;
        body["namespace"] = "list_test";
        body["user_id"] = "list_user";
        body["title"] = "Session " + std::to_string(i);
        auto r = cli.Post("/api/v1/memory/sessions", Admin(),
                          body.dump(), "application/json");
        ASSERT_TRUE(r && r->status == 201);
    }

    // List all (MEM05: filtered to list_user's sessions).
    auto all = cli.Get("/api/v1/memory/sessions?namespace=list_test&user_id=list_user", Admin());
    ASSERT_TRUE(all && all->status == 200);
    auto body = json::parse(all->body);
    EXPECT_EQ(body["total_count"].get<int>(), 4);
    EXPECT_EQ(body["sessions"].size(), 4u);

    // Unique session IDs
    std::set<std::string> ids;
    for (const auto& s : body["sessions"]) {
        ids.insert(s["session_id"].get<std::string>());
    }
    EXPECT_EQ(ids.size(), 4u);
}

// ------------------------------------------------------------------
// Test 9: Memory search endpoint
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, MemorySearchEndpoint) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("msearch_test").ok());

    // Create session + interactions
    json sess;
    sess["namespace"] = "msearch_test";
    sess["user_id"] = "searcher";
    auto sr = cli.Post("/api/v1/memory/sessions", Admin(),
                       sess.dump(), "application/json");
    ASSERT_TRUE(sr && sr->status == 201);
    std::string sid = json::parse(sr->body)["session_id"].get<std::string>();

    json inter;
    inter["namespace"] = "msearch_test";
    inter["query_text"] = "What is vector search?";
    inter["response_text"] = "Vector search finds similar embeddings.";
    auto wr = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                       Admin(), inter.dump(), "application/json");
    ASSERT_TRUE(wr && wr->status == 201);

    // Search
    json search;
    search["namespace"] = "msearch_test";
    search["query"] = "vector";
    auto res = cli.Post("/api/v1/memory/search", Admin(),
                        search.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
}

// ------------------------------------------------------------------
// Test 10: Upload different file types
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, UploadDifferentFileTypes) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("mime_test").ok());

    // Text
    httplib::MultipartFormDataItems txt = {
        {"file", "Plain text.", "doc.txt", "text/plain"},
    };
    auto r1 = cli.Post("/api/v1/namespaces/mime_test/documents", Admin(), txt);
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->status, 201);

    // Markdown
    httplib::MultipartFormDataItems md = {
        {"file", "# Title\n\nContent.", "doc.md", "text/markdown"},
    };
    auto r2 = cli.Post("/api/v1/namespaces/mime_test/documents", Admin(), md);
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 201);

    // CSV
    httplib::MultipartFormDataItems csv = {
        {"file", "a,b,c\n1,2,3", "data.csv", "text/csv"},
    };
    auto r3 = cli.Post("/api/v1/namespaces/mime_test/documents", Admin(), csv);
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 201);
}

// ------------------------------------------------------------------
// Test 11: Unicode content handling
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, UnicodeContentHandling) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("unicode_test").ok());

    // Chinese document
    httplib::MultipartFormDataItems items = {
        {"file", "人工智能是计算机科学的一个分支。深度学习使用多层神经网络。",
         "chinese.txt", "text/plain"},
    };
    auto u = cli.Post("/api/v1/namespaces/unicode_test/documents", Admin(), items);
    ASSERT_TRUE(u && u->status == 201);

    // Memory session with unicode
    json sess;
    sess["namespace"] = "unicode_test";
    sess["user_id"] = "用户一";
    sess["title"] = "中文测试";
    auto sr = cli.Post("/api/v1/memory/sessions", Admin(),
                       sess.dump(), "application/json");
    ASSERT_TRUE(sr && sr->status == 201);
    std::string sid = json::parse(sr->body)["session_id"].get<std::string>();

    // Write unicode interaction
    json inter;
    inter["namespace"] = "unicode_test";
    inter["query_text"] = "什么是机器学习？";
    inter["response_text"] = "机器学习是人工智能的子领域。";
    auto wr = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                       Admin(), inter.dump(), "application/json");
    ASSERT_TRUE(wr && wr->status == 201);

    // Verify unicode preserved
    auto detail = cli.Get("/api/v1/memory/sessions/" + sid +
                          "?namespace=unicode_test&user_id=用户一", Admin());
    ASSERT_TRUE(detail && detail->status == 200);
    auto djson = json::parse(detail->body);
    EXPECT_EQ(djson["session"]["user_id"], "用户一");
    EXPECT_EQ(djson["session"]["title"], "中文测试");

    bool found_cn = false;
    for (const auto& log : djson["interactions"]) {
        if (log["content"].get<std::string>().find("机器学习") != std::string::npos)
            found_cn = true;
    }
    EXPECT_TRUE(found_cn);
}

// ------------------------------------------------------------------
// Test 12: Large query truncation
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, LargeQueryTruncation) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("trunc_test").ok());

    std::string large_query(3000, 'a');
    json body;
    body["query"] = large_query;
    body["namespace"] = "trunc_test";
    body["top_k"] = 5;
    auto r = cli.Post("/api/v1/query", Admin(), body.dump(), "application/json");
    ASSERT_TRUE(r);
    // Should handle gracefully
    EXPECT_TRUE(r->status == 200 || r->status == 503)
        << "Large query: " << r->status;
}

// ------------------------------------------------------------------
// Test 13: Upload empty file
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, UploadEmptyFile) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("empty_test").ok());

    httplib::MultipartFormDataItems items = {
        {"file", "", "empty.txt", "text/plain"},
    };
    auto r = cli.Post("/api/v1/namespaces/empty_test/documents", Admin(), items);
    ASSERT_TRUE(r);
    // Either accepted or rejected, no crash
    EXPECT_TRUE(r->status == 201 || r->status == 400)
        << "Empty file: " << r->status << " " << r->body;
}

// ------------------------------------------------------------------
// Test 14: Multiple uploads then query
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, MultipleUploadsThenQuery) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("multi_test").ok());

    for (const auto& [name, content] : std::vector<std::pair<std::string, std::string>>{
        {"physics.txt", "Quantum mechanics describes atomic particles."},
        {"biology.txt", "DNA replication is fundamental to cell division."},
        {"history.txt", "The Renaissance was a cultural rebirth in Europe."},
    }) {
        httplib::MultipartFormDataItems items = {
            {"file", content, name, "text/plain"},
        };
        auto r = cli.Post("/api/v1/namespaces/multi_test/documents",
                          Admin(), items);
        ASSERT_TRUE(r && r->status == 201) << "Upload " << name << " failed";
    }

    // Query (docs pending)
    json q;
    q["query"] = "quantum mechanics";
    q["namespace"] = "multi_test";
    auto qr = cli.Post("/api/v1/query", Admin(), q.dump(), "application/json");
    ASSERT_TRUE(qr);
    EXPECT_TRUE(qr->status == 200 || qr->status == 503);
}

// ------------------------------------------------------------------
// Test 15: Session delete and re-create
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, SessionDeleteRecreate) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("recreate_test").ok());

    // Create + write
    json body;
    body["namespace"] = "recreate_test";
    body["user_id"] = "tester";
    auto r1 = cli.Post("/api/v1/memory/sessions", Admin(),
                       body.dump(), "application/json");
    ASSERT_TRUE(r1 && r1->status == 201);
    std::string sid1 = json::parse(r1->body)["session_id"].get<std::string>();

    json inter;
    inter["namespace"] = "recreate_test";
    inter["query_text"] = "Original query";
    inter["response_text"] = "Original response";
    auto wr = cli.Post("/api/v1/memory/sessions/" + sid1 + "/interactions",
                       Admin(), inter.dump(), "application/json");
    ASSERT_TRUE(wr && wr->status == 201);

    // Delete
    auto del = cli.Delete("/api/v1/memory/sessions/" + sid1 +
                          "?namespace=recreate_test&user_id=tester", Admin());
    ASSERT_TRUE(del && del->status == 200);

    // Re-create
    auto r2 = cli.Post("/api/v1/memory/sessions", Admin(),
                       body.dump(), "application/json");
    ASSERT_TRUE(r2 && r2->status == 201);
    std::string sid2 = json::parse(r2->body)["session_id"].get<std::string>();

    EXPECT_NE(sid1, sid2);

    // New session should be empty
    auto detail = cli.Get("/api/v1/memory/sessions/" + sid2 +
                          "?namespace=recreate_test&user_id=tester", Admin());
    ASSERT_TRUE(detail && detail->status == 200);
    auto djson = json::parse(detail->body);
    EXPECT_EQ(djson["interactions"].size(), 0u);
}

// ------------------------------------------------------------------
// Test 16: Memory interaction with query_type and latency
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, MemoryInteractionMetadata) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("meta_test").ok());

    json sess;
    sess["namespace"] = "meta_test";
    sess["user_id"] = "meta_user";
    sess["title"] = "Metadata Session";
    auto sr = cli.Post("/api/v1/memory/sessions", Admin(),
                       sess.dump(), "application/json");
    ASSERT_TRUE(sr && sr->status == 201);
    std::string sid = json::parse(sr->body)["session_id"].get<std::string>();

    json inter;
    inter["namespace"] = "meta_test";
    inter["query_text"] = "Search test";
    inter["response_text"] = "Search results";
    inter["query_type"] = "search";
    inter["latency_ms"] = 42;
    auto wr = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                       Admin(), inter.dump(), "application/json");
    ASSERT_TRUE(wr && wr->status == 201);

    // Verify session detail (MEM05: owner = meta_user).
    auto detail = cli.Get("/api/v1/memory/sessions/" + sid +
                          "?namespace=meta_test&user_id=meta_user", Admin());
    ASSERT_TRUE(detail && detail->status == 200);
    auto djson = json::parse(detail->body);
    EXPECT_EQ(djson["session"]["title"], "Metadata Session");
    EXPECT_GE(djson["interactions"].size(), 2u);
}

// ------------------------------------------------------------------
// Test 17: Inject with multiple turns
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, InjectMultipleTurns) {
    httplib::Client cli("127.0.0.1", port_);
    ASSERT_TRUE(harness_->Admit("inject_test").ok());

    // Create session
    json sess;
    sess["namespace"] = "inject_test";
    auto sr = cli.Post("/api/v1/memory/sessions", Admin(),
                       sess.dump(), "application/json");
    ASSERT_TRUE(sr && sr->status == 201);
    std::string sid = json::parse(sr->body)["session_id"].get<std::string>();

    // Write 5 turns
    for (int i = 0; i < 5; ++i) {
        json body;
        body["namespace"] = "inject_test";
        body["query_text"] = "Turn " + std::to_string(i) + " question";
        body["response_text"] = "Turn " + std::to_string(i) + " answer";
        auto wr = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                           Admin(), body.dump(), "application/json");
        ASSERT_TRUE(wr && wr->status == 201);
    }

    // Inject → should return last 3 turns (config: inject_recent_turns=3)
    json inj;
    inj["namespace"] = "inject_test";
    inj["session_id"] = sid;
    auto res = cli.Post("/api/v1/memory/inject", Admin(),
                        inj.dump(), "application/json");
    ASSERT_TRUE(res && res->status == 200);
    auto rjson = json::parse(res->body);
    EXPECT_EQ(rjson["turn_count"].get<int>(), 3);
    EXPECT_GT(rjson["token_count_approx"].get<int>(), 0);

    std::string ctx = rjson["context_text"].get<std::string>();
    EXPECT_NE(ctx.find("Turn 2"), std::string::npos);
    EXPECT_NE(ctx.find("Turn 4"), std::string::npos);
    EXPECT_EQ(ctx.find("Turn 0"), std::string::npos)
        << "Old turns should be excluded";
}

// ------------------------------------------------------------------
// Test 18: Namespace not found for all route groups
// ------------------------------------------------------------------
TEST_F(FullServerE2ETest, NamespaceNotFoundAllRoutes) {
    httplib::Client cli("127.0.0.1", port_);

    // Upload to nonexistent namespace → 404
    httplib::MultipartFormDataItems items = {
        {"file", "test", "test.txt", "text/plain"},
    };
    auto r1 = cli.Post("/api/v1/namespaces/ghost_ns/documents", Admin(), items);
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->status, 404);

    // Query nonexistent namespace → 404
    auto r2 = cli.Post("/api/v1/query", Admin(),
                       json({{"query","test"},{"namespace","ghost_ns"}}).dump(),
                       "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 404);
}

}  // namespace
}  // namespace cortrix
