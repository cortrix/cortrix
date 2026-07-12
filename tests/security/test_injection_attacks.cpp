/// @file test_injection_attacks.cpp
/// @brief Security tests: SQL/FTS5 injection, path traversal, JSON injection, null bytes
///
/// Validates that CortrixStore and HTTP APIs properly sanitize user input
/// to prevent injection attacks through search queries, metadata fields,
/// and file paths.

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>

#include "httplib.h"
#include <nlohmann/json.hpp>

// [wire⑤c follow-up] MVP CortrixNamespace(Manager) + hnswlib headers removed;
// the HTTP fixtures stand on the F05 pool via the shared NsPoolHarness.
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/config/config.h"
#include "unit/namespace_authz_test_helper.h"
#include "unit/ns_pool_test_helper.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/memory/memory_routes.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/upload/upload_handler.h"
#include "cortrix/server/routes/document_routes.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// Direct Store-Level Injection Tests (no HTTP)
// ============================================================

class StoreInjectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_sec_inject_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);
        db_path_ = (test_dir_ / "test.db").string();
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);

        // Seed a document and block for search tests
        CortrixDoc doc;
        doc.source_type = "test";
        doc.source_path = "test_doc.txt";
        ASSERT_EQ(store_->doc_create(doc), 0);
        doc_id_ = doc.doc_id;

        auto blob = BlockBuild(kBlockFile, kLevelL2, "safe content here", "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc_id_;
        block.chunk_index = 0;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = "safe content here";
        ASSERT_EQ(store_->block_insert(block), 0);
    }

    void TearDown() override {
        store_->Close();
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::string db_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
    std::string doc_id_;  // D-I6: doc_id is a ULID string
};

// SEC-INJ-001: FTS5 operator injection via search_fulltext
TEST_F(StoreInjectionTest, Fts5OperatorInjection) {
    std::vector<SearchResult> results;

    // These would be dangerous without SanitizeFts5Query:
    // "* NOT *" could match everything, "OR" could bypass intent
    std::vector<std::string> injection_payloads = {
        "* NOT *",
        "content OR 1=1",
        "safe AND NOT safe",
        "NEAR(safe, content, 1)",
        "content*",
        "\"safe\" OR \"1\"=\"1\"",
        "content) OR (1=1",
        "^safe",
    };

    for (const auto& payload : injection_payloads) {
        results.clear();
        int ret = store_->search_fulltext(payload, 10, results);
        // Must not crash, must not return SQL error
        EXPECT_EQ(ret, 0) << "FTS5 injection crash for: " << payload;
        // Results should be limited to legitimate matches, not wildcard match-all
    }
}

// SEC-INJ-002: SQL injection via metadata fields
TEST_F(StoreInjectionTest, SqlInjectionViaMetadata) {
    // Attempt SQL injection through doc metadata fields
    CortrixDoc doc;
    doc.source_type = "test'; DROP TABLE cortrix_docs;--";
    doc.source_path = "path' OR '1'='1";
    doc.content_hash = "hash\" UNION SELECT * FROM cortrix_docs--";
    doc.metadata_json = R"({"key":"'; DELETE FROM cortrix_blocks;--"})";
    doc.title = "title' OR 1=1--";

    // Should store safely (parameterized queries prevent injection)
    int ret = store_->doc_create(doc);
    EXPECT_EQ(ret, 0);
    EXPECT_FALSE(doc.doc_id.empty());  // D-I6: ULID assigned on create

    // Verify the SQL-injection strings are stored literally, not executed
    CortrixDoc retrieved;
    ret = store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(retrieved.source_type, "test'; DROP TABLE cortrix_docs;--");
    EXPECT_EQ(retrieved.source_path, "path' OR '1'='1");
    EXPECT_EQ(retrieved.title, "title' OR 1=1--");

    // Verify the original doc table is still intact
    int64_t count = 0;
    store_->doc_count(&count);
    EXPECT_GE(count, 2); // our seeded doc + injection doc
}

// SEC-INJ-003: LIKE pattern injection in search_metadata
TEST_F(StoreInjectionTest, LikePatternInjection) {
    std::vector<SearchResult> results;

    // LIKE-specific injection patterns
    std::vector<std::string> like_payloads = {
        "%",          // wildcard match all
        "_%",         // single char + wildcard
        "\\",         // escape character
        "' OR 1=1--", // classic SQL injection
        "content%' UNION SELECT 1,2,3--",
    };

    for (const auto& payload : like_payloads) {
        results.clear();
        int ret = store_->search_metadata(payload, 10, results);
        // search_metadata returns -1 (not implemented), verify no crash
        EXPECT_TRUE(ret == 0 || ret == -1)
            << "LIKE injection crash for: " << payload;
    }
}

// SEC-INJ-004: Null byte injection in string fields
TEST_F(StoreInjectionTest, NullByteInjection) {
    // Null bytes can truncate strings in C, potentially bypassing checks
    std::string null_source = std::string("normal\0malicious", 16);
    std::string null_path = std::string("safe/path\0../../etc/passwd", 26);

    CortrixDoc doc;
    doc.source_type = null_source;
    doc.source_path = null_path;

    int ret = store_->doc_create(doc);
    EXPECT_EQ(ret, 0);

    CortrixDoc retrieved;
    ret = store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(ret, 0);
    // SQLite stores the full string including null bytes via blob binding,
    // or truncates at null. Either is safe — the key is no crash/injection.
}

// SEC-INJ-005: JSON injection in metadata_json field
TEST_F(StoreInjectionTest, JsonInjectionInMetadata) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";

    // Attempt to inject nested JSON that could confuse parsing
    doc.metadata_json = R"({"key":"value","injected":true,"nested":{"drop":"table"}})";

    int ret = store_->doc_create(doc);
    EXPECT_EQ(ret, 0);

    CortrixDoc retrieved;
    ret = store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(ret, 0);

    // Verify metadata is stored as-is (the DB treats it as opaque text)
    auto j = json::parse(retrieved.metadata_json);
    EXPECT_EQ(j["key"], "value");
    EXPECT_EQ(j["injected"], true);
}

// ============================================================
// HTTP-Level Injection Tests
// ============================================================

class HttpInjectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_sec_http_inj_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        config_.ns.data_dir = tmp_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;
        config_.memory.inject_recent_turns = 3;
        config_.memory.inject_max_tokens = 2000;
        config_.memory.default_ttl_seconds = 0;
        config_.memory.max_sessions_per_namespace = 100;
        config_.memory.max_interactions_per_session = 500;

        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);
        ASSERT_TRUE(harness_->Admit("default").ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        test_key_ = "sec-test-key-inject";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "sec-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        // ARCHITECTURE V6: grant the test key access to "default" (the namespace
        // the HTTP injection probes target) via a REAL PermissionService seam,
        // seeding "default" as owned by the key's tenant.
        authz_ = std::make_unique<cortrix::test::NamespaceAuthzHarness>(
            *auth_, &harness_->ipool(), "sec-tenant",
            std::vector<std::string>{"default"});

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();
        spc_mgr_ = std::make_unique<TestSPCManager>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_mgr_);

        svr_ = std::make_unique<httplib::Server>();
        RegisterDocumentRoutes(*svr_, *handler_, harness_->ipool(), *auth_);
        RegisterQueryRoutes(*svr_, *auth_, harness_->ipool(), *embedder_,
                            *classifier_, *fusion_);
        RegisterMemoryRoutes(*svr_, *auth_, harness_->ipool(), *spc_mgr_,
                             *embedder_, *classifier_, *fusion_,
                             config_.memory);

        port_ = 19100 + (getpid() % 500);
        svr_thread_ = std::thread([this] { svr_->listen("127.0.0.1", port_); });

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

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    class TestSPCManager : public SPCManager {
    public:
        TestSPCManager() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    CortrixConfig config_;
    fs::path tmp_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<cortrix::test::NamespaceAuthzHarness> authz_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<TestSPCManager> spc_mgr_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string test_key_;
    int port_;
};

// SEC-INJ-006: SQL injection via HTTP query endpoint
TEST_F(HttpInjectionTest, SqlInjectionViaQueryEndpoint) {
    httplib::Client cli("127.0.0.1", port_);

    std::vector<std::string> injection_queries = {
        "'; DROP TABLE cortrix_docs;--",
        "1 UNION SELECT * FROM cortrix_docs",
        "' OR '1'='1",
        "1; DELETE FROM cortrix_blocks WHERE 1=1",
    };

    for (const auto& payload : injection_queries) {
        json body;
        body["query"] = payload;
        body["namespace"] = "default";
        body["top_k"] = 5;

        auto res = cli.Post("/api/v1/query", AuthHeaders(),
                            body.dump(), "application/json");
        ASSERT_TRUE(res) << "No response for injection: " << payload;
        // Should return 200 (empty results) or 503 (degraded), NOT 500
        EXPECT_TRUE(res->status == 200 || res->status == 503)
            << "Unexpected status " << res->status
            << " for injection: " << payload;
    }
}

// SEC-INJ-007: Path traversal in document upload filename
TEST_F(HttpInjectionTest, PathTraversalInUpload) {
    httplib::Client cli("127.0.0.1", port_);

    std::vector<std::string> traversal_filenames = {
        "../../etc/passwd",
        "../../../root/.ssh/id_rsa",
        "..\\..\\windows\\system32\\config\\sam",
        "normal/../../../etc/shadow",
        "/etc/passwd",
        "file\0../../etc/passwd",
    };

    for (const auto& filename : traversal_filenames) {
        httplib::MultipartFormDataItems items = {
            {"file", "test content for path traversal check",
             filename, "text/plain"},
        };

        auto res = cli.Post("/api/v1/namespaces/default/documents",
                            AuthHeaders(), items);
        ASSERT_TRUE(res) << "No response for filename: " << filename;
        // Server should either accept safely (sanitizing the path)
        // or reject with 400. Must NOT write to traversed paths.
        EXPECT_TRUE(res->status == 201 || res->status == 400 || res->status == 422)
            << "Unexpected status " << res->status
            << " for traversal: " << filename;
    }
}

}  // namespace
}  // namespace cortrix
