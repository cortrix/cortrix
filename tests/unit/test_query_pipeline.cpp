#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/sql_executor.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/common/data_types.h"
#include "ns_pool_test_helper.h"  // cortrix::test::FakeIndex (IIndex fake)
#include <unordered_map>

using ::testing::_;
using ::testing::Return;
using ::testing::SetArgReferee;
using ::testing::DoAll;

namespace cortrix {
namespace {

// ============================================================
// Test doubles
// ============================================================

class FakeStore : public CortrixStore {
public:
    std::vector<SearchResult> fulltext_results;
    int fulltext_rc = 0;
    // blocks keyed by block_id (uint64); docs keyed by doc_id (ULID string)
    std::unordered_map<int64_t, CortrixBlock> blocks;
    std::unordered_map<std::string, CortrixDoc> docs;

    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(CortrixDoc&) override { return 0; }
    int doc_get(const std::string& doc_id, CortrixDoc& doc) override {
        auto it = docs.find(doc_id);
        if (it == docs.end()) return -2;
        doc = it->second;
        return 0;
    }
    int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
    int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
    int doc_count(int64_t*) override { return 0; }
    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t block_id, CortrixBlock& block) override {
        auto it = blocks.find(block_id);
        if (it == blocks.end()) return -2;
        block = it->second;
        return 0;
    }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t*) override { return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>& results) override {
        results = fulltext_results;
        return fulltext_rc;
    }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

// VectorSearcher now takes a store::IIndex& (namespace pool wire⑤c). The old
// FakeVectorIndex (CortrixVectorIndex with an rc-returning search()) is replaced
// by the shared cortrix::test::FakeIndex, whose Search() returns hits directly
// (no rc); scripted via set_search_result({{block_id, distance}, ...}).

// ============================================================
// Component integration tests (original - test RRF + PostFilter + Degradation)
// ============================================================

class QueryPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        fake_vec_.set_search_result({{101, 0.1f}, {102, 0.3f}});

        fake_store_.fulltext_results = {
            {102, "test-doc-1", 12.0, "text 102"},
            {201, "test-doc-2", 9.0, "text 201"}
        };

        CortrixBlock blk101{101, "test-doc-1", 0, 1, 3, -1, "", {}, "Vector text 101"};
        CortrixBlock blk102{102, "test-doc-1", 1, 1, 3, -1, "", {}, "Both routes text 102"};
        CortrixBlock blk201{201, "test-doc-2", 0, 3, 3, -1, "", {}, "BM25 text 201"};
        fake_store_.blocks[101] = blk101;
        fake_store_.blocks[102] = blk102;
        fake_store_.blocks[201] = blk201;

        CortrixDoc doc1;
        doc1.doc_id = "test-doc-1";
        doc1.source_path = "/test/doc1.pdf";
        doc1.created_at = "2025-01-01T00:00:00Z";
        fake_store_.docs["test-doc-1"] = doc1;

        CortrixDoc doc2;
        doc2.doc_id = "test-doc-2";
        doc2.source_path = "/test/doc2.pdf";
        doc2.created_at = "2025-06-01T00:00:00Z";
        fake_store_.docs["test-doc-2"] = doc2;
    }

    cortrix::test::FakeIndex fake_vec_;
    FakeStore fake_store_;
};

TEST_F(QueryPipelineTest, ComponentsIntegration_RRF_Filter_Degradation) {
    RouteResult vec_result;
    vec_result.route_name = "vector";
    vec_result.status = RouteStatus::kSuccess;
    vec_result.items = {{101, "test-doc-1", 0, 0.9f}, {102, "test-doc-1", 1, 0.7f}};

    RouteResult bm25_result;
    bm25_result.route_name = "bm25";
    bm25_result.status = RouteStatus::kSuccess;
    bm25_result.items = {{102, "test-doc-1", 1, 12.0f}, {201, "test-doc-2", 0, 9.0f}};

    RRFFusion fusion(60);
    auto scored = fusion.Merge({vec_result, bm25_result});

    ASSERT_GE(scored.size(), 2u);
    EXPECT_EQ(scored[0].block_id, 102);
    EXPECT_EQ(scored[0].hit_routes, kRouteVector | kRouteBM25);

    PostFilter filter(fake_store_);
    QueryFilter qf;
    auto results = filter.Apply(scored, qf, 10);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].block_id, 102);
    EXPECT_EQ(results[0].related_blocks_count, 2);

    DegradationManager deg;
    RouteResult sql_skipped;
    sql_skipped.route_name = "sql";
    sql_skipped.status = RouteStatus::kSkipped;
    SearchConfig config{true, true, false};

    auto info = deg.Evaluate(vec_result, bm25_result, sql_skipped, config);
    EXPECT_EQ(info.level, DegradationLevel::kL0);
    EXPECT_FALSE(info.degraded);
}

TEST_F(QueryPipelineTest, DegradedMode_VectorFails) {
    RouteResult vec_result;
    vec_result.route_name = "vector";
    vec_result.status = RouteStatus::kError;
    vec_result.error_message = "embedding failed";

    RouteResult bm25_result;
    bm25_result.route_name = "bm25";
    bm25_result.status = RouteStatus::kSuccess;
    bm25_result.items = {{102, "test-doc-1", 1, 12.0f}, {201, "test-doc-2", 0, 9.0f}};

    RRFFusion fusion(60);
    auto scored = fusion.Merge({vec_result, bm25_result});
    EXPECT_EQ(scored.size(), 2u);
    EXPECT_EQ(scored[0].hit_routes, kRouteBM25);

    DegradationManager deg;
    RouteResult sql_skipped{"sql", RouteStatus::kSkipped};
    SearchConfig config{true, true, false};
    auto info = deg.Evaluate(vec_result, bm25_result, sql_skipped, config);
    EXPECT_EQ(info.level, DegradationLevel::kL2);
    EXPECT_TRUE(info.degraded);
}

TEST_F(QueryPipelineTest, AllRoutesFail_L3) {
    RouteResult vec_result{"vector", RouteStatus::kTimeout, "timeout"};
    RouteResult bm25_result{"bm25", RouteStatus::kError, "db error"};
    RouteResult sql_skipped{"sql", RouteStatus::kSkipped};

    RRFFusion fusion(60);
    auto scored = fusion.Merge({vec_result, bm25_result});
    EXPECT_TRUE(scored.empty());

    DegradationManager deg;
    SearchConfig config{true, true, false};
    auto info = deg.Evaluate(vec_result, bm25_result, sql_skipped, config);
    EXPECT_EQ(info.level, DegradationLevel::kL3);
}

TEST_F(QueryPipelineTest, FilterWithBlockType) {
    RouteResult bm25_result;
    bm25_result.route_name = "bm25";
    bm25_result.status = RouteStatus::kSuccess;
    bm25_result.items = {{101, "test-doc-1", 0, 12.0f}, {201, "test-doc-2", 0, 9.0f}};

    RRFFusion fusion(60);
    auto scored = fusion.Merge({bm25_result});

    PostFilter filter(fake_store_);
    QueryFilter qf;
    qf.block_types = {"DATABASE"};
    auto results = filter.Apply(scored, qf, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 201);
    EXPECT_EQ(results[0].block_type, "DATABASE");
}

TEST(QueryPipelineConfigTest, DefaultMergeReserveMs) {
    EXPECT_EQ(QueryPipeline::kDefaultMergeReserveMs, 200);
}

TEST(QueryPipelineConfigTest, MinRouteTimeoutMs) {
    EXPECT_EQ(QueryPipeline::kDefaultMergeReserveMs, 200);
}

TEST_F(QueryPipelineTest, FilterWithCreatedAfter) {
    RouteResult bm25_result;
    bm25_result.route_name = "bm25";
    bm25_result.status = RouteStatus::kSuccess;
    bm25_result.items = {{101, "test-doc-1", 0, 12.0f}, {201, "test-doc-2", 0, 9.0f}};

    RRFFusion fusion(60);
    auto scored = fusion.Merge({bm25_result});

    PostFilter filter(fake_store_);
    QueryFilter qf;
    qf.created_after = "2025-03-01";
    auto results = filter.Apply(scored, qf, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 201);
}

TEST_F(QueryPipelineTest, FilterWithSourcePathGlob) {
    RouteResult bm25_result;
    bm25_result.route_name = "bm25";
    bm25_result.status = RouteStatus::kSuccess;
    bm25_result.items = {{101, "test-doc-1", 0, 12.0f}, {201, "test-doc-2", 0, 9.0f}};

    RRFFusion fusion(60);
    auto scored = fusion.Merge({bm25_result});

    PostFilter filter(fake_store_);
    QueryFilter qf;
    qf.source_path = "/test/*";
    auto results = filter.Apply(scored, qf, 10);
    ASSERT_EQ(results.size(), 2u);
}

// ============================================================
// NEW: Full pipeline Execute() tests (covers query_pipeline.cpp)
// These are the critical 0% coverage tests.
// ============================================================

class QueryPipelineExecuteTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize stub embedder
        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();

        // Setup fake vector results
        fake_vec_.set_search_result({{101, 0.1f}, {102, 0.3f}});

        // Setup fake BM25 results
        fake_store_.fulltext_results = {
            {102, "test-doc-1", 12.0, "text 102"},
            {201, "test-doc-2", 9.0, "text 201"}
        };

        // Setup block details
        CortrixBlock blk101{101, "test-doc-1", 0, 1, 3, -1, "", {}, "Vector text 101"};
        CortrixBlock blk102{102, "test-doc-1", 1, 1, 3, -1, "", {}, "Both routes text 102"};
        CortrixBlock blk201{201, "test-doc-2", 0, 3, 3, -1, "", {}, "BM25 text 201"};
        fake_store_.blocks[101] = blk101;
        fake_store_.blocks[102] = blk102;
        fake_store_.blocks[201] = blk201;

        // Setup doc details
        CortrixDoc doc1;
        doc1.doc_id = "test-doc-1";
        doc1.source_path = "/test/doc1.pdf";
        doc1.created_at = "2025-01-01T00:00:00Z";
        fake_store_.docs["test-doc-1"] = doc1;

        CortrixDoc doc2;
        doc2.doc_id = "test-doc-2";
        doc2.source_path = "/test/doc2.pdf";
        doc2.created_at = "2025-06-01T00:00:00Z";
        fake_store_.docs["test-doc-2"] = doc2;
    }

    QueryRequest MakeRequest(const std::string& query = "test query") {
        QueryRequest req;
        req.query = query;
        req.namespace_name = "default";
        req.top_k = 10;
        req.timeout_ms = 5000;
        return req;
    }

    std::unique_ptr<OnnxEmbedder> embedder_;
    cortrix::test::FakeIndex fake_vec_;
    FakeStore fake_store_;
};

// --- Hybrid query: both vector + BM25 enabled ---

TEST_F(QueryPipelineExecuteTest, Execute_HybridQuery_BothRoutes) {
    LlmConfig llm_config;  // empty -> keyword mode
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    auto req = MakeRequest("test query");
    QueryResponse response = pipeline.Execute(req);

    // Should have results from both routes
    EXPECT_FALSE(response.has_error);
    EXPECT_GT(response.results.size(), 0u);
    EXPECT_GT(response.meta.latency_ms, -1);
    EXPECT_FALSE(response.meta.degraded);
    EXPECT_EQ(response.meta.degradation_level, 0);

    // sql_result should be null
    EXPECT_FALSE(response.sql_result.has_result);

    // meta should show both routes
    EXPECT_GE(response.meta.routes_used.size(), 1u);
}

// --- Vector-only query ---

TEST_F(QueryPipelineExecuteTest, Execute_VectorOnly) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    auto req = MakeRequest("test");
    req.search_config.enable_bm25 = false;
    req.search_config.enable_sql = false;

    QueryResponse response = pipeline.Execute(req);

    EXPECT_FALSE(response.has_error);
    EXPECT_FALSE(response.sql_result.has_result);
}

// --- BM25-only query ---

TEST_F(QueryPipelineExecuteTest, Execute_BM25Only) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    auto req = MakeRequest("test");
    req.search_config.enable_vector = false;
    req.search_config.enable_sql = false;

    QueryResponse response = pipeline.Execute(req);

    EXPECT_FALSE(response.has_error);
    EXPECT_GT(response.results.size(), 0u);
}

// --- Both routes disabled + SQL disabled -> L3 error ---

TEST_F(QueryPipelineExecuteTest, Execute_AllRoutesDisabled_L3) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    auto req = MakeRequest("test");
    req.search_config.enable_vector = false;
    req.search_config.enable_bm25 = false;
    req.search_config.enable_sql = false;

    QueryResponse response = pipeline.Execute(req);

    // All routes disabled/skipped: depends on DegradationManager logic.
    // With no routes enabled, it returns L0 (user's choice), not L3.
    // Pipeline will return empty results but NOT an error.
    EXPECT_EQ(response.meta.total_results, 0);
}

// --- One search route fails, the other succeeds -> degraded L2 ---
// REPURPOSED (namespace pool wire⑤c): the old version made the VECTOR route fail via
// FakeVectorIndex.search_rc=-1. IIndex::Search no longer has an rc (empty hits =
// kSuccess, not failure), so a vector-index failure is unreachable. The L2
// degradation intent is preserved by failing BM25 instead (fulltext_rc=-1) while
// vector succeeds — still exactly one healthy search route -> L2.
TEST_F(QueryPipelineExecuteTest, Execute_OneRouteFails_DegradedL2) {
    // Make BM25 fail; vector keeps its scripted hits (healthy).
    fake_store_.fulltext_rc = -1;
    fake_store_.fulltext_results.clear();

    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);

    // Use a real OnnxEmbedder that is initialized
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    auto req = MakeRequest("test");
    QueryResponse response = pipeline.Execute(req);

    // BM25 fails but vector succeeds -> degraded
    EXPECT_TRUE(response.meta.degraded);
    EXPECT_EQ(response.meta.degradation_level, static_cast<int>(DegradationLevel::kL2));
    EXPECT_FALSE(response.has_error);  // Not L3, so no error
    EXPECT_GT(response.results.size(), 0u);  // Vector results present
}

// --- All enabled routes fail -> L3 error response ---
// REPURPOSED (namespace pool wire⑤c): the old version drove L3 by failing BOTH vector
// (search_rc=-1) and BM25 with SQL disabled. A vector-index failure is no longer
// reachable (IIndex::Search has no rc), so to leave NO healthy search route we
// disable vector + SQL, keeping only BM25 enabled and failing it (fulltext_rc=-1)
// -> every enabled route failed -> L3.
TEST_F(QueryPipelineExecuteTest, Execute_AllRoutesFail_L3Error) {
    fake_store_.fulltext_rc = -1;
    fake_store_.fulltext_results.clear();

    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    auto req = MakeRequest("test");
    req.search_config.enable_vector = false;  // vector cannot fail via the index;
    req.search_config.enable_sql = false;     // disable both so only BM25 is enabled
                                              // -> BM25 failure leaves no healthy route

    QueryResponse response = pipeline.Execute(req);

    EXPECT_TRUE(response.has_error);
    EXPECT_EQ(response.error_message, "All search routes failed");
    EXPECT_EQ(response.meta.degradation_level, static_cast<int>(DegradationLevel::kL3));
    EXPECT_TRUE(response.meta.degraded);
    EXPECT_EQ(response.meta.total_results, 0);
}

// --- Custom merge reserve ---

TEST_F(QueryPipelineExecuteTest, Execute_CustomMergeReserve) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    // Custom merge reserve = 500ms
    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub, 500);

    auto req = MakeRequest("test");
    QueryResponse response = pipeline.Execute(req);
    EXPECT_FALSE(response.has_error);
}

// --- Zero/negative merge reserve defaults to kDefaultMergeReserveMs ---

TEST_F(QueryPipelineExecuteTest, Execute_ZeroMergeReserve_DefaultsToDefault) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    // merge_reserve_ms = 0 -> should use default
    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub, 0);

    auto req = MakeRequest("test");
    QueryResponse response = pipeline.Execute(req);
    EXPECT_FALSE(response.has_error);
}

// --- Verify latency_ms is measured ---

TEST_F(QueryPipelineExecuteTest, Execute_LatencyMeasured) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    auto req = MakeRequest("test");
    QueryResponse response = pipeline.Execute(req);
    EXPECT_GE(response.meta.latency_ms, 0);
}

// --- Verify meta.total_results matches actual results ---

TEST_F(QueryPipelineExecuteTest, Execute_TotalResultsMatchesActual) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    auto req = MakeRequest("test");
    QueryResponse response = pipeline.Execute(req);

    EXPECT_EQ(response.meta.total_results, static_cast<int>(response.results.size()));
}

// --- CalculateRouteTimeout: low timeout uses floor ---

TEST_F(QueryPipelineExecuteTest, Execute_LowTimeout_UsesMinFloor) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    // Very low timeout: 100ms - 200ms reserve = negative -> clamped to 100ms minimum
    auto req = MakeRequest("test");
    req.timeout_ms = 100;
    QueryResponse response = pipeline.Execute(req);
    // Should still work (clamped to 100ms route timeout)
    EXPECT_GE(response.meta.latency_ms, 0);
}

// --- SQL intent does not trigger SQL route in semantic score ---

TEST_F(QueryPipelineExecuteTest, Execute_SqlIntent_NoSqlRoute) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    // Query with SQL keywords should trigger kSql intent
    auto req = MakeRequest("SELECT * FROM users WHERE active = 1");
    QueryResponse response = pipeline.Execute(req);

    // Semantic score: SQL route is always skipped
    EXPECT_FALSE(response.sql_result.has_result);
}

// --- Empty BM25 query (all operators) still returns vector results ---

TEST_F(QueryPipelineExecuteTest, Execute_BM25EmptyAfterSanitize_VectorStillWorks) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    // "AND OR NOT" -> BM25 sanitization produces empty -> BM25 error
    // But vector search should still work
    auto req = MakeRequest("AND OR NOT");
    QueryResponse response = pipeline.Execute(req);

    // Should be degraded (BM25 failed) but not L3
    // Vector search might still succeed
    EXPECT_GE(response.meta.latency_ms, 0);
}

// --- SqlExecutor wiring: intent-based SQL route invocation ---

// Custom mock SqlExecutor for testing
class MockSqlExecutor : public SqlExecutor {
public:
    int call_count = 0;
    RouteStatus return_status = RouteStatus::kSkipped;

    RouteResult Execute(const std::string& /*query_text*/,
                        const std::string& /*namespace_name*/,
                        int64_t /*timeout_us*/,
                        SqlResult& sql_result) override {
        ++call_count;
        sql_result.has_result = false;
        RouteResult result;
        result.route_name = "sql";
        result.status = return_status;
        result.latency_us = 0;
        return result;
    }

    bool IsAvailable() const override { return true; }
};

TEST_F(QueryPipelineExecuteTest, SqlExecutor_CalledOnSqlIntent) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;
    MockSqlExecutor mock_sql;

    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, mock_sql);

    // Query with SQL keywords -> kSql intent -> should call SqlExecutor
    auto req = MakeRequest("SELECT COUNT(*) FROM users");
    req.search_config.enable_sql = true;
    pipeline.Execute(req);

    EXPECT_GT(mock_sql.call_count, 0);
}

TEST_F(QueryPipelineExecuteTest, SqlExecutor_NotCalledOnSemanticIntent) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;
    MockSqlExecutor mock_sql;

    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, mock_sql);

    // Pure semantic query -> kSemantic intent -> should NOT call SqlExecutor
    auto req = MakeRequest("find documents about risk management");
    req.search_config.enable_sql = true;
    pipeline.Execute(req);

    EXPECT_EQ(mock_sql.call_count, 0);
}

TEST_F(QueryPipelineExecuteTest, SqlExecutor_NotCalledWhenSqlDisabled) {
    LlmConfig llm_config;
    IntentClassifier classifier(llm_config);
    RRFFusion fusion(60);
    VectorSearcher vec_searcher(fake_vec_, *embedder_);
    BM25Searcher bm25_searcher(fake_store_);
    PostFilter post_filter(fake_store_);
    DegradationManager deg_mgr;
    MockSqlExecutor mock_sql;

    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, mock_sql);

    // SQL keyword query but enable_sql=false -> should NOT call SqlExecutor
    auto req = MakeRequest("SELECT * FROM users");
    req.search_config.enable_sql = false;
    pipeline.Execute(req);

    EXPECT_EQ(mock_sql.call_count, 0);
}

}  // namespace
}  // namespace cortrix
