/// @file test_e2e_error_recovery.cpp
/// @brief E2E: Error recovery and graceful degradation tests
///
/// Tests that the system handles error conditions gracefully without crashing:
///   1. Query on nonexistent namespace → proper error, no crash
///   2. HNSW search on empty index → returns empty results gracefully
///   3. Duplicate upload with same content_hash → dedup via doc_find_by_hash
///   4. Memory search with nonexistent session → graceful error

#include <gtest/gtest.h>
#include <filesystem>

#include <nlohmann/json.hpp>

// Storage
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/cortrix_blob_local.h"
#include "cortrix/store/phnsw.h"
// Query
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/query_response.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/sql_executor.h"
// SPC
#include "cortrix/spc/onnx_embedder.h"
// Memory
#include "cortrix/memory/memory_store.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace cortrix::store;  // PHnsw / PhnswConfig / IndexStats live in cortrix::store

// ==================================================================
// Error Recovery Test Fixture
// ==================================================================

class ErrorRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_e2e_err_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);

        db_path_ = (test_dir_ / "cortrix.db").string();
        blob_dir_ = (test_dir_ / "blobs").string();
        vec_path_ = (test_dir_ / "vec").string();  // PHnsw uses a unit data dir (not a file)

        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);

        blob_ = std::make_unique<CortrixBlobLocal>(blob_dir_);

        PhnswConfig cfg;
        cfg.dim = 128;
        cfg.max_elements = 10000;
        vec_ = std::make_unique<PHnsw>(vec_path_, cfg);  // ctor runs Recover()

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        ASSERT_TRUE(embedder_->Init().ok());
    }

    void TearDown() override {
        store_->Close();
        store_.reset();
        blob_.reset();
        vec_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::string db_path_, blob_dir_, vec_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
    std::unique_ptr<CortrixBlobLocal> blob_;
    std::unique_ptr<PHnsw> vec_;
    std::unique_ptr<OnnxEmbedder> embedder_;
};

// Query pipeline on an empty store with no blocks → should degrade gracefully, not crash
TEST_F(ErrorRecoveryTest, QueryNonexistentNamespace) {
    // Build a query pipeline against the empty store (no documents, no blocks)
    LlmConfig llm_cfg;
    IntentClassifier classifier(llm_cfg);
    RRFFusion fusion;
    VectorSearcher vec_searcher(*vec_, *embedder_);
    BM25Searcher bm25_searcher(*store_);
    PostFilter post_filter(*store_);
    DegradationManager deg_mgr;
    SqlExecutorStub sql_stub;

    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    QueryRequest req;
    req.query = "nonexistent data in empty namespace";
    req.namespace_name = "nonexistent_ns";
    req.top_k = 5;
    req.timeout_ms = 5000;
    req.Normalize();

    // Should not crash — either returns empty results or degrades
    QueryResponse resp = pipeline.Execute(req);

    // Accept either: no error with empty results, or degraded response
    if (!resp.has_error) {
        EXPECT_EQ(resp.results.size(), 0u)
            << "Empty store should return 0 results";
    } else {
        EXPECT_TRUE(resp.meta.degraded)
            << "Error response should be marked as degraded";
    }
}

// HNSW search on empty index → should return empty results gracefully
TEST_F(ErrorRecoveryTest, HnswEmptyIndexDegrades) {
    // The index is initialized but has 0 vectors
    ASSERT_EQ(vec_->GetStats().vector_count, 0u);

    // Create a query vector
    EmbeddingResult query_emb;
    Status s = embedder_->Embed("test query on empty index", &query_emb);
    ASSERT_TRUE(s.ok());

    // Search should not crash and should return empty results.
    // PHnsw returns an empty vector for an empty index (graceful degradation).
    auto results = vec_->Search(query_emb.vector.data(), /*top_k=*/10);
    EXPECT_EQ(results.size(), 0u) << "Empty index should return 0 results";
}

// Upload same content_hash twice → doc_find_by_hash returns existing doc (dedup)
TEST_F(ErrorRecoveryTest, DuplicateUploadDedup) {
    const std::string content_hash = "sha256_dedup_test_hash";

    // First upload
    CortrixDoc doc1;
    doc1.source_type = "http_upload";
    doc1.source_path = "file_v1.txt";
    doc1.content_hash = content_hash;
    doc1.file_size = 100;
    doc1.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc1), 0);
    ASSERT_FALSE(doc1.doc_id.empty());  // [D-I6] doc_id is a ULID string now

    // Second upload attempt with same hash → find existing
    CortrixDoc existing;
    int rc = store_->doc_find_by_hash(content_hash, existing);
    ASSERT_EQ(rc, 0) << "doc_find_by_hash should find existing doc";
    EXPECT_EQ(existing.doc_id, doc1.doc_id)
        << "Should return the same doc_id for duplicate hash";
    EXPECT_EQ(existing.content_hash, content_hash);

    // Verify doc_find_by_hash with nonexistent hash returns not-found
    CortrixDoc not_found;
    rc = store_->doc_find_by_hash("sha256_does_not_exist", not_found);
    EXPECT_NE(rc, 0) << "Should return error for nonexistent hash";
}

// Memory search with nonexistent session → graceful error
TEST_F(ErrorRecoveryTest, MemorySearchNoSession) {
    // Create MemoryStore backed by the CortrixStore
    MemoryStore mem_store(*store_);
    std::string mem_db_path = (test_dir_ / "memory.db").string();
    Status s = mem_store.Init(mem_db_path);
    ASSERT_TRUE(s.ok()) << s.message();

    // Try to get a nonexistent session
    MemorySession session;
    s = mem_store.SessionGet("nonexistent-session-id-1234", session);
    EXPECT_FALSE(s.ok()) << "Should return error for nonexistent session";

    // Try to list interactions for nonexistent session → should return empty, not crash
    std::vector<InteractionLog> interactions;
    s = mem_store.InteractionListBySession("nonexistent-session-id-1234", interactions);
    // Should succeed with empty list or return not-found — either way no crash
    if (s.ok()) {
        EXPECT_EQ(interactions.size(), 0u)
            << "Nonexistent session should have 0 interactions";
    }

    // Try to get recent interactions for nonexistent session
    std::vector<InteractionLog> recent;
    s = mem_store.InteractionGetRecent("nonexistent-session-id-1234", 5, recent);
    if (s.ok()) {
        EXPECT_EQ(recent.size(), 0u);
    }
}

}  // namespace
}  // namespace cortrix
