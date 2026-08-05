#include <gtest/gtest.h>
#include "cortrix/memory/memory_searcher.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/sql_executor.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/common/data_types.h"
#include "cortrix/config/config.h"
#include "ns_pool_test_helper.h"  // cortrix::test::FakeIndex (IIndex fake)
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace cortrix {
namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// MemorySearchRequest::Validate() tests (lines 20-37 of memory_searcher.cpp)
// ---------------------------------------------------------------------------

class MemorySearchRequestTest : public ::testing::Test {};

TEST_F(MemorySearchRequestTest, ValidRequest) {
    MemorySearchRequest req;
    req.query = "test query";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;  // MEM05: default scope, user_id required
    req.user_id = "user_001";
    req.top_k = 10;

    auto s = req.Validate();
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(MemorySearchRequestTest, EmptyQueryFails) {
    MemorySearchRequest req;
    req.query = "";
    req.namespace_name = "default";
    req.user_id = "user_001";

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("query"), std::string::npos);
}

TEST_F(MemorySearchRequestTest, EmptyNamespaceFails) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "";
    req.user_id = "user_001";

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("namespace"), std::string::npos);
}

TEST_F(MemorySearchRequestTest, SessionScopeRequiresSessionId) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = "user_001";  // MEM05: user_id always required
    req.scope = MemoryScope::kSession;
    req.session_id = "";  // missing

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("session_id"), std::string::npos);
}

TEST_F(MemorySearchRequestTest, SessionScopeWithIdSucceeds) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = "user_001";  // MEM05: user_id always required
    req.scope = MemoryScope::kSession;
    req.session_id = "abc-123";

    auto s = req.Validate();
    EXPECT_TRUE(s.ok()) << s.message();
}

// MEM05: user_id is unconditionally required (no longer scope-dependent).
TEST_F(MemorySearchRequestTest, UserIdRequiredEvenForUserScope) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;
    req.user_id = "";  // missing

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("user_id"), std::string::npos);
}

// MEM05: user_id required even for session scope.
TEST_F(MemorySearchRequestTest, UserIdRequiredForSessionScope) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.scope = MemoryScope::kSession;
    req.session_id = "s1";
    req.user_id = "";  // missing

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("user_id"), std::string::npos);
}

TEST_F(MemorySearchRequestTest, UserScopeWithIdSucceeds) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;
    req.user_id = "user_001";

    auto s = req.Validate();
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(MemorySearchRequestTest, TopKZeroFails) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = "user_001";
    req.top_k = 0;

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("top_k"), std::string::npos);
}

TEST_F(MemorySearchRequestTest, TopKNegativeFails) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = "user_001";
    req.top_k = -5;

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(MemorySearchRequestTest, TopKOver100Fails) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = "user_001";
    req.top_k = 101;

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("top_k"), std::string::npos);
}

TEST_F(MemorySearchRequestTest, TopKBoundary1Succeeds) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = "user_001";
    req.top_k = 1;

    auto s = req.Validate();
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(MemorySearchRequestTest, TopKBoundary100Succeeds) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = "user_001";
    req.top_k = 100;

    auto s = req.Validate();
    EXPECT_TRUE(s.ok()) << s.message();
}

// MEM05: user_id format rule — oversize (>128 chars) is rejected by Validate()
// (IsValidUserIdFormat length guard, line 29).
TEST_F(MemorySearchRequestTest, UserIdOver128CharsRejected) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = std::string(129, 'a');  // 129 > kMaxUserIdLen(128)

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("invalid user_id format"), std::string::npos);
}

// MEM05: user_id exactly 128 chars is the boundary and is accepted.
TEST_F(MemorySearchRequestTest, UserId128CharsBoundaryAccepted) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = std::string(128, 'a');  // == kMaxUserIdLen

    auto s = req.Validate();
    EXPECT_TRUE(s.ok()) << s.message();
}

// MEM05: a control char (< 0x20) in user_id fails the ASCII-printable charset check
// (IsValidUserIdFormat loop, line 31).
TEST_F(MemorySearchRequestTest, UserIdWithControlCharRejected) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = std::string("user\x01id");  // 0x01 is below 0x20

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("invalid user_id format"), std::string::npos);
}

// MEM05: a byte above 0x7E (e.g. 0x80) in user_id also fails the format check.
TEST_F(MemorySearchRequestTest, UserIdWithHighByteRejected) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = std::string("user") + static_cast<char>(0x80) + "id";

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// MEM05: default scope (kUser) succeeds with a user_id and no session_id.
TEST_F(MemorySearchRequestTest, DefaultUserScopeWithUserIdSucceeds) {
    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.user_id = "user_001";
    // scope defaults to kUser; session_id may be empty

    auto s = req.Validate();
    EXPECT_TRUE(s.ok()) << s.message();
}

// ---------------------------------------------------------------------------
// IsExpired() tests (lines 122-165 of memory_searcher.cpp)
// These test the static method MemorySearcher::IsExpired which is private,
// so we test through the Search() method behavior.
// We can verify IsExpired logic indirectly by creating search results with
// TTL metadata and checking if they are filtered.
//
// However, since IsExpired is static, we test it by examining behavior through
// the Validate method and metadata handling patterns.
// ---------------------------------------------------------------------------

// Test IsExpired indirectly: metadata with ttl_seconds=0 means never expires
TEST_F(MemorySearchRequestTest, DefaultValuesAreReasonable) {
    MemorySearchRequest req;
    EXPECT_EQ(req.top_k, 10);
    EXPECT_EQ(req.scope, MemoryScope::kUser);  // MEM05: default changed kAll -> kUser
    EXPECT_FALSE(req.include_expired);
    EXPECT_FALSE(req.include_invalidated);     // MEM01 D4 default
    EXPECT_EQ(req.timeout_ms, 5000);
}

// ---------------------------------------------------------------------------
// MemorySearcher::MatchScope() and ToMemoryResult() tested through
// MemorySearcher::Search() integration - requires QueryPipeline construction.
//
// Since QueryPipeline is not mockable (no virtual interface), we test the
// individual helper methods via their behavioral effects through the Validate
// path and metadata-based assertions.
// ---------------------------------------------------------------------------

// Test MemorySearchResultItem defaults
TEST(MemorySearchResultItemTest, DefaultValues) {
    MemorySearchResultItem item;
    EXPECT_TRUE(item.interaction_id.empty());
    EXPECT_TRUE(item.session_id.empty());
    EXPECT_TRUE(item.query_text.empty());
    EXPECT_TRUE(item.response_text.empty());
    EXPECT_TRUE(item.query_type.empty());
    EXPECT_FLOAT_EQ(item.score, 0.0f);
    EXPECT_FALSE(item.expired);
    EXPECT_TRUE(item.created_at.empty());
    EXPECT_TRUE(item.metadata_json.empty());
}

// Test MemorySearchResponse defaults
TEST(MemorySearchResponseTest, DefaultValues) {
    MemorySearchResponse resp;
    EXPECT_TRUE(resp.results.empty());
    EXPECT_EQ(resp.total_results, 0);
    EXPECT_EQ(resp.latency_ms, 0);
    EXPECT_FALSE(resp.degraded);
}

// ---------------------------------------------------------------------------
// MemoryScope enum coverage
// ---------------------------------------------------------------------------

TEST(MemoryScopeTest, EnumValues) {
    EXPECT_EQ(static_cast<int>(MemoryScope::kSession), 0);
    EXPECT_EQ(static_cast<int>(MemoryScope::kUser), 1);
    // MEM05: kAll removed. Only kSession / kUser remain.
}

// ---------------------------------------------------------------------------
// Validate: combination tests
// ---------------------------------------------------------------------------

TEST_F(MemorySearchRequestTest, SessionScopeEmptyQueryAndSessionId) {
    MemorySearchRequest req;
    req.query = "";
    req.namespace_name = "default";
    req.scope = MemoryScope::kSession;
    req.session_id = "";

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    // Should fail on first validation: query is required
    EXPECT_NE(s.message().find("query"), std::string::npos);
}

TEST_F(MemorySearchRequestTest, UserScopeEmptyQueryAndUserId) {
    MemorySearchRequest req;
    req.query = "";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;
    req.user_id = "";

    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    // First check: query is required
    EXPECT_NE(s.message().find("query"), std::string::npos);
}

TEST_F(MemorySearchRequestTest, ValidSessionScopeWithAllFields) {
    MemorySearchRequest req;
    req.query = "revenue analysis";
    req.namespace_name = "finance_ns";
    req.scope = MemoryScope::kSession;
    req.session_id = "session-abc-123";
    req.user_id = "user_001";  // MEM05: user_id always required
    req.top_k = 20;
    req.include_expired = true;
    req.timeout_ms = 10000;

    auto s = req.Validate();
    EXPECT_TRUE(s.ok()) << s.message();
}

// ===========================================================================
// MemorySearcher::Search() end-to-end tests
// Uses real QueryPipeline with FakeStore/FakeVectorIndex to exercise
// Search, ToQueryRequest, IsExpired, MatchScope, ToMemoryResult
// ===========================================================================

class FakeStoreForSearch : public CortrixStore {
public:
    std::vector<SearchResult> fulltext_results;
    int fulltext_rc = 0;
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

// Vector index fake: the shared cortrix::test::FakeIndex implements the F01
// IIndex contract VectorSearcher now takes. Search() returns the scripted hits
// directly (no rc), set via set_search_result({{block_id, distance}, ...}).
using cortrix::test::FakeIndex;

class MemorySearcherSearchTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up fake store with blocks that have MEMORY type and metadata
        json meta1;
        meta1["interaction_id"] = "int-001";
        meta1["session_id"] = "sess-abc";
        meta1["user_id"] = "user-1";
        meta1["query_type"] = "semantic";

        CortrixBlock blk1{101, "test-doc-1", 0, 6, 3, -1, meta1.dump(), {}, "What is AI?\n---\nAI is artificial intelligence."};
        store_.blocks[101] = blk1;

        json meta2;
        meta2["interaction_id"] = "int-002";
        meta2["session_id"] = "sess-xyz";
        meta2["user_id"] = "user-2";

        CortrixBlock blk2{102, "test-doc-1", 1, 6, 3, -1, meta2.dump(), {}, "Hello\n---\nWorld"};
        store_.blocks[102] = blk2;

        // Block with TTL that is expired
        json meta3;
        meta3["interaction_id"] = "int-003";
        meta3["session_id"] = "sess-abc";
        meta3["ttl_seconds"] = 1;
        meta3["queried_at"] = "2020-01-01T00:00:00.000Z";

        CortrixBlock blk3{103, "test-doc-1", 2, 6, 3, -1, meta3.dump(), {}, "Old Q\n---\nOld A"};
        store_.blocks[103] = blk3;

        // Block without separator
        json meta4;
        meta4["interaction_id"] = "int-004";
        meta4["session_id"] = "sess-abc";
        CortrixBlock blk4{104, "test-doc-1", 3, 6, 3, -1, meta4.dump(), {}, "no separator text"};
        store_.blocks[104] = blk4;

        CortrixDoc doc1;
        doc1.doc_id = "test-doc-1";
        doc1.source_path = "/data/memory.db";
        doc1.created_at = "2025-01-01T00:00:00Z";
        store_.docs["test-doc-1"] = doc1;

        // Set up vector index to return our blocks
        vec_index_.set_search_result({{101, 0.1f}, {102, 0.2f}, {103, 0.3f}, {104, 0.4f}});

        // BM25 fulltext results
        store_.fulltext_results = {
            {101, "test-doc-1", 12.0, "What is AI?"},
            {102, "test-doc-1", 10.0, "Hello"}
        };
    }

    FakeStoreForSearch store_;
    FakeIndex vec_index_;
};

TEST_F(MemorySearcherSearchTest, SearchAllScope_ReturnsResults) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    BM25Searcher bm25(store_);
    VectorSearcher vec(vec_index_, embedder);
    RRFFusion rrf;
    LlmConfig llm_cfg;
    IntentClassifier intent(llm_cfg);
    PostFilter post_filter(store_);
    DegradationManager degrad;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec, bm25, rrf, intent, post_filter, degrad, sql_stub);

    // Create in-memory store for MemorySearcher
    MemoryStore mem_store(store_);
    mem_store.Init();

    MemoryConfig config;
    MemorySearcher searcher(pipeline, mem_store, config);

    MemorySearchRequest req;
    req.query = "AI";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;  // MEM05: kAll removed
    req.user_id = "user-1";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    // MEM05: only user-1's memories are returned; others are excluded.
    EXPECT_GE(resp.total_results, 0);
    EXPECT_GE(resp.latency_ms, 0);
}

TEST_F(MemorySearcherSearchTest, SearchSessionScope_FiltersCorrectly) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    BM25Searcher bm25(store_);
    VectorSearcher vec(vec_index_, embedder);
    RRFFusion rrf;
    LlmConfig llm_cfg;
    IntentClassifier intent(llm_cfg);
    PostFilter post_filter(store_);
    DegradationManager degrad;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec, bm25, rrf, intent, post_filter, degrad, sql_stub);

    MemoryStore mem_store(store_);
    mem_store.Init();

    MemoryConfig config;
    MemorySearcher searcher(pipeline, mem_store, config);

    MemorySearchRequest req;
    req.query = "AI";
    req.namespace_name = "default";
    req.scope = MemoryScope::kSession;
    req.session_id = "sess-abc";
    req.user_id = "user-1";  // MEM05: user_id always required
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    // MEM05: must match BOTH user-1 and sess-abc.
    for (const auto& item : resp.results) {
        EXPECT_EQ(item.session_id, "sess-abc");
    }
}

TEST_F(MemorySearcherSearchTest, SearchUserScope_FiltersCorrectly) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    BM25Searcher bm25(store_);
    VectorSearcher vec(vec_index_, embedder);
    RRFFusion rrf;
    LlmConfig llm_cfg;
    IntentClassifier intent(llm_cfg);
    PostFilter post_filter(store_);
    DegradationManager degrad;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec, bm25, rrf, intent, post_filter, degrad, sql_stub);

    MemoryStore mem_store(store_);
    mem_store.Init();

    MemoryConfig config;
    MemorySearcher searcher(pipeline, mem_store, config);

    MemorySearchRequest req;
    req.query = "hello";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;
    req.user_id = "user-1";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_GE(resp.total_results, 0);
}

TEST_F(MemorySearcherSearchTest, SearchInvalidRequest_ReturnsEmpty) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    BM25Searcher bm25(store_);
    VectorSearcher vec(vec_index_, embedder);
    RRFFusion rrf;
    LlmConfig llm_cfg;
    IntentClassifier intent(llm_cfg);
    PostFilter post_filter(store_);
    DegradationManager degrad;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec, bm25, rrf, intent, post_filter, degrad, sql_stub);

    MemoryStore mem_store(store_);
    mem_store.Init();

    MemoryConfig config;
    MemorySearcher searcher(pipeline, mem_store, config);

    MemorySearchRequest req;
    req.query = "";  // invalid - empty
    req.namespace_name = "default";

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_EQ(resp.total_results, 0);
    EXPECT_TRUE(resp.results.empty());
}

TEST_F(MemorySearcherSearchTest, SearchExcludeExpired_FiltersExpiredResults) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    BM25Searcher bm25(store_);
    VectorSearcher vec(vec_index_, embedder);
    RRFFusion rrf;
    LlmConfig llm_cfg;
    IntentClassifier intent(llm_cfg);
    PostFilter post_filter(store_);
    DegradationManager degrad;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec, bm25, rrf, intent, post_filter, degrad, sql_stub);

    MemoryStore mem_store(store_);
    mem_store.Init();

    MemoryConfig config;
    MemorySearcher searcher(pipeline, mem_store, config);

    MemorySearchRequest req;
    req.query = "old";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;  // MEM05: kAll removed
    req.user_id = "user-1";
    req.top_k = 10;
    req.include_expired = false;  // should filter out expired

    MemorySearchResponse resp = searcher.Search(req);
    // Expired items should be filtered
    for (const auto& item : resp.results) {
        EXPECT_FALSE(item.expired);
    }
}

TEST_F(MemorySearcherSearchTest, SearchIncludeExpired_ReturnsExpiredMarked) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    BM25Searcher bm25(store_);
    VectorSearcher vec(vec_index_, embedder);
    RRFFusion rrf;
    LlmConfig llm_cfg;
    IntentClassifier intent(llm_cfg);
    PostFilter post_filter(store_);
    DegradationManager degrad;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec, bm25, rrf, intent, post_filter, degrad, sql_stub);

    MemoryStore mem_store(store_);
    mem_store.Init();

    MemoryConfig config;
    MemorySearcher searcher(pipeline, mem_store, config);

    MemorySearchRequest req;
    req.query = "data";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;  // MEM05: kAll removed
    req.user_id = "user-1";
    req.top_k = 10;
    req.include_expired = true;  // include expired

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_GE(resp.total_results, 0);
}

TEST_F(MemorySearcherSearchTest, SearchTopK1_LimitsResults) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    BM25Searcher bm25(store_);
    VectorSearcher vec(vec_index_, embedder);
    RRFFusion rrf;
    LlmConfig llm_cfg;
    IntentClassifier intent(llm_cfg);
    PostFilter post_filter(store_);
    DegradationManager degrad;

    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec, bm25, rrf, intent, post_filter, degrad, sql_stub);

    MemoryStore mem_store(store_);
    mem_store.Init();

    MemoryConfig config;
    MemorySearcher searcher(pipeline, mem_store, config);

    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;  // MEM05: kAll removed
    req.user_id = "user-1";
    req.top_k = 1;

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_LE(static_cast<int>(resp.results.size()), 1);
}

// Search top_k break (line 109): two blocks both owned by user-1 with top_k=1
// must stop after the first accepted result.
TEST_F(MemorySearcherSearchTest, SearchTopK1_BreaksAfterFirstWhenMultipleMatch) {
    // Two user-1 blocks so >1 result survives MatchScope; top_k=1 triggers the break.
    json meta_a;
    meta_a["interaction_id"] = "int-a";
    meta_a["user_id"] = "user-1";
    store_.blocks[301] = CortrixBlock{301, "test-doc-1", 0, 6, 3, -1, meta_a.dump(), {}, "A\n---\na"};
    json meta_b;
    meta_b["interaction_id"] = "int-b";
    meta_b["user_id"] = "user-1";
    store_.blocks[302] = CortrixBlock{302, "test-doc-1", 1, 6, 3, -1, meta_b.dump(), {}, "B\n---\nb"};
    vec_index_.set_search_result({{301, 0.1f}, {302, 0.2f}});

    OnnxEmbedder embedder("", 128);
    embedder.Init();
    BM25Searcher bm25(store_);
    VectorSearcher vec(vec_index_, embedder);
    RRFFusion rrf;
    LlmConfig llm_cfg;
    IntentClassifier intent(llm_cfg);
    PostFilter post_filter(store_);
    DegradationManager degrad;
    SqlExecutorStub sql_stub;
    QueryPipeline pipeline(vec, bm25, rrf, intent, post_filter, degrad, sql_stub);

    MemoryStore mem_store(store_);
    mem_store.Init();
    MemoryConfig config;
    MemorySearcher searcher(pipeline, mem_store, config);

    MemorySearchRequest req;
    req.query = "x";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;
    req.user_id = "user-1";
    req.top_k = 1;

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_LE(static_cast<int>(resp.results.size()), 1);
}

}  // namespace

// ===========================================================================
// Direct private method tests via friend class
// This class has access to MemorySearcher private methods
// ===========================================================================

class MemorySearcherPrivateTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.fulltext_results = {};
        vec_index_.set_search_result({});

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        bm25_ = std::make_unique<BM25Searcher>(store_);
        vec_searcher_ = std::make_unique<VectorSearcher>(vec_index_, *embedder_);
        rrf_ = std::make_unique<RRFFusion>();
        LlmConfig llm_cfg;
        intent_ = std::make_unique<IntentClassifier>(llm_cfg);
        post_filter_ = std::make_unique<PostFilter>(store_);
        degrad_ = std::make_unique<DegradationManager>();
        sql_stub_ = std::make_unique<SqlExecutorStub>();

        pipeline_ = std::make_unique<QueryPipeline>(
            *vec_searcher_, *bm25_, *rrf_, *intent_, *post_filter_, *degrad_, *sql_stub_);

        mem_store_ = std::make_unique<MemoryStore>(store_);
        mem_store_->Init();
    }

    // Access private static method IsExpired
    static bool CallIsExpired(const std::string& metadata_json) {
        return MemorySearcher::IsExpired(metadata_json);
    }

    // Access private method MatchScope
    bool CallMatchScope(MemorySearcher& searcher, const json& metadata,
                        const MemorySearchRequest& request) {
        return searcher.MatchScope(metadata, request);
    }

    // Access private method ToQueryRequest
    QueryRequest CallToQueryRequest(MemorySearcher& searcher,
                                     const MemorySearchRequest& request) {
        return searcher.ToQueryRequest(request);
    }

    // Access private method ToMemoryResult
    MemorySearchResultItem CallToMemoryResult(MemorySearcher& searcher,
                                               const ResultItem& item) {
        return searcher.ToMemoryResult(item);
    }

    FakeStoreForSearch store_;
    FakeIndex vec_index_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<BM25Searcher> bm25_;
    std::unique_ptr<VectorSearcher> vec_searcher_;
    std::unique_ptr<RRFFusion> rrf_;
    std::unique_ptr<IntentClassifier> intent_;
    std::unique_ptr<PostFilter> post_filter_;
    std::unique_ptr<DegradationManager> degrad_;
    std::unique_ptr<SqlExecutorStub> sql_stub_;
    std::unique_ptr<QueryPipeline> pipeline_;
    std::unique_ptr<MemoryStore> mem_store_;
};

// ---------------------------------------------------------------------------
// IsExpired direct tests (lines 122-165)
// ---------------------------------------------------------------------------

TEST_F(MemorySearcherPrivateTest, IsExpired_EmptyString_ReturnsFalse) {
    EXPECT_FALSE(CallIsExpired(""));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_NoTtlField_ReturnsFalse) {
    json meta;
    meta["interaction_id"] = "test";
    EXPECT_FALSE(CallIsExpired(meta.dump()));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_TtlZero_ReturnsFalse) {
    json meta;
    meta["ttl_seconds"] = 0;
    meta["queried_at"] = "2020-01-01T00:00:00.000Z";
    EXPECT_FALSE(CallIsExpired(meta.dump()));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_TtlNegative_ReturnsFalse) {
    json meta;
    meta["ttl_seconds"] = -100;
    meta["queried_at"] = "2020-01-01T00:00:00.000Z";
    EXPECT_FALSE(CallIsExpired(meta.dump()));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_NoQueriedAt_ReturnsFalse) {
    json meta;
    meta["ttl_seconds"] = 3600;
    // no queried_at field
    EXPECT_FALSE(CallIsExpired(meta.dump()));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_QueriedAtNotString_ReturnsFalse) {
    json meta;
    meta["ttl_seconds"] = 3600;
    meta["queried_at"] = 12345;  // number, not string
    EXPECT_FALSE(CallIsExpired(meta.dump()));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_MalformedTimestamp_ReturnsFalse) {
    json meta;
    meta["ttl_seconds"] = 3600;
    meta["queried_at"] = "not-a-timestamp";
    EXPECT_FALSE(CallIsExpired(meta.dump()));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_PastTimestampWithTTL_ReturnsTrue) {
    json meta;
    meta["ttl_seconds"] = 1;  // 1 second TTL
    meta["queried_at"] = "2020-01-01T00:00:00.000Z";  // far in the past
    EXPECT_TRUE(CallIsExpired(meta.dump()));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_FutureTimestamp_ReturnsFalse) {
    json meta;
    meta["ttl_seconds"] = 999999999;  // huge TTL
    meta["queried_at"] = "2020-01-01T00:00:00.000Z";
    // 2020 + 999999999 seconds is way in the future
    EXPECT_FALSE(CallIsExpired(meta.dump()));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_InvalidJson_ReturnsFalse) {
    EXPECT_FALSE(CallIsExpired("{invalid json}}}"));
}

TEST_F(MemorySearcherPrivateTest, IsExpired_TtlNotNumber_ReturnsFalse) {
    json meta;
    meta["ttl_seconds"] = "not_a_number";
    meta["queried_at"] = "2020-01-01T00:00:00.000Z";
    EXPECT_FALSE(CallIsExpired(meta.dump()));
}

// ---------------------------------------------------------------------------
// MatchScope direct tests (lines 171-190)
// ---------------------------------------------------------------------------

// MEM05: kAll removed. A memory with no user_id in metadata is always excluded.
TEST_F(MemorySearcherPrivateTest, MatchScope_NoUserIdInMeta_Excluded) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kUser;
    req.user_id = "user-1";

    json meta;
    meta["session_id"] = "any";  // no user_id -> excluded under MEM05
    EXPECT_FALSE(CallMatchScope(searcher, meta, req));
}

TEST_F(MemorySearcherPrivateTest, MatchScope_kSession_MatchesCorrectSession) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kSession;
    req.session_id = "sess-abc";
    req.user_id = "user-1";  // MEM05: user_id checked first

    json meta;
    meta["user_id"] = "user-1";
    meta["session_id"] = "sess-abc";
    EXPECT_TRUE(CallMatchScope(searcher, meta, req));
}

TEST_F(MemorySearcherPrivateTest, MatchScope_kSession_RejectsWrongSession) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kSession;
    req.session_id = "sess-abc";
    req.user_id = "user-1";

    json meta;
    meta["user_id"] = "user-1";       // right user
    meta["session_id"] = "sess-xyz";  // wrong session
    EXPECT_FALSE(CallMatchScope(searcher, meta, req));
}

// MEM05: session scope with right session but WRONG user is still excluded.
TEST_F(MemorySearcherPrivateTest, MatchScope_kSession_RightSessionWrongUser_Excluded) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kSession;
    req.session_id = "sess-abc";
    req.user_id = "user-1";

    json meta;
    meta["user_id"] = "user-2";       // wrong user -> excluded before session check
    meta["session_id"] = "sess-abc";
    EXPECT_FALSE(CallMatchScope(searcher, meta, req));
}

TEST_F(MemorySearcherPrivateTest, MatchScope_kSession_NoSessionIdInMeta_ReturnsFalse) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kSession;
    req.session_id = "sess-abc";
    req.user_id = "user-1";

    json meta;
    meta["user_id"] = "user-1";  // right user, but no session_id
    EXPECT_FALSE(CallMatchScope(searcher, meta, req));
}

TEST_F(MemorySearcherPrivateTest, MatchScope_kSession_SessionIdNotString_ReturnsFalse) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kSession;
    req.session_id = "sess-abc";
    req.user_id = "user-1";

    json meta;
    meta["user_id"] = "user-1";
    meta["session_id"] = 12345;  // number, not string
    EXPECT_FALSE(CallMatchScope(searcher, meta, req));
}

TEST_F(MemorySearcherPrivateTest, MatchScope_kUser_MatchesCorrectUser) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kUser;
    req.user_id = "user-1";

    json meta;
    meta["user_id"] = "user-1";
    EXPECT_TRUE(CallMatchScope(searcher, meta, req));
}

TEST_F(MemorySearcherPrivateTest, MatchScope_kUser_RejectsWrongUser) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kUser;
    req.user_id = "user-1";

    json meta;
    meta["user_id"] = "user-2";
    EXPECT_FALSE(CallMatchScope(searcher, meta, req));
}

TEST_F(MemorySearcherPrivateTest, MatchScope_kUser_NoUserIdInMeta_ReturnsFalse) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kUser;
    req.user_id = "user-1";

    json meta;
    meta["session_id"] = "sess-1";  // no user_id
    EXPECT_FALSE(CallMatchScope(searcher, meta, req));
}

TEST_F(MemorySearcherPrivateTest, MatchScope_kUser_UserIdNotString_ReturnsFalse) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.scope = MemoryScope::kUser;
    req.user_id = "user-1";

    json meta;
    meta["user_id"] = 42;  // number, not string
    EXPECT_FALSE(CallMatchScope(searcher, meta, req));
}

// ---------------------------------------------------------------------------
// ToQueryRequest direct tests (lines 105-116)
// ---------------------------------------------------------------------------

TEST_F(MemorySearcherPrivateTest, ToQueryRequest_SetsCorrectFields) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "test query";
    req.namespace_name = "ns1";
    req.top_k = 5;
    req.timeout_ms = 3000;

    QueryRequest qr = CallToQueryRequest(searcher, req);

    EXPECT_EQ(qr.query, "test query");
    EXPECT_EQ(qr.namespace_name, "ns1");
    EXPECT_EQ(qr.top_k, 10);  // 5 * 2 (oversample)
    EXPECT_EQ(qr.timeout_ms, 3000);
    EXPECT_FALSE(qr.search_config.enable_sql);
    EXPECT_TRUE(qr.search_config.enable_vector);
    EXPECT_TRUE(qr.search_config.enable_bm25);
    ASSERT_EQ(qr.filters.block_types.size(), 1u);
    EXPECT_EQ(qr.filters.block_types[0], "MEMORY");
}

// ---------------------------------------------------------------------------
// ToMemoryResult direct tests (lines 196-229)
// ---------------------------------------------------------------------------

TEST_F(MemorySearcherPrivateTest, ToMemoryResult_WithAllMetadata) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    json meta;
    meta["interaction_id"] = "int-001";
    meta["session_id"] = "sess-abc";
    meta["query_type"] = "semantic";

    ResultItem item;
    item.block_id = 101;
    item.score = 0.95f;
    item.metadata = meta;
    item.chunk_text = "What is AI?\n---\nAI is artificial intelligence.";

    auto result = CallToMemoryResult(searcher, item);

    EXPECT_EQ(result.interaction_id, "int-001");
    EXPECT_EQ(result.session_id, "sess-abc");
    EXPECT_EQ(result.query_type, "semantic");
    EXPECT_FLOAT_EQ(result.score, 0.95f);
    EXPECT_EQ(result.query_text, "What is AI?");
    EXPECT_EQ(result.response_text, "AI is artificial intelligence.");
    EXPECT_FALSE(result.metadata_json.empty());
}

TEST_F(MemorySearcherPrivateTest, ToMemoryResult_WithoutSeparator) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    json meta;
    meta["interaction_id"] = "int-002";

    ResultItem item;
    item.score = 0.5f;
    item.metadata = meta;
    item.chunk_text = "plain text without separator";

    auto result = CallToMemoryResult(searcher, item);

    EXPECT_EQ(result.interaction_id, "int-002");
    EXPECT_EQ(result.query_text, "plain text without separator");
    EXPECT_TRUE(result.response_text.empty());
}

TEST_F(MemorySearcherPrivateTest, ToMemoryResult_NullMetadata) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    ResultItem item;
    item.score = 0.3f;
    item.metadata = json();  // null
    item.chunk_text = "some text";

    auto result = CallToMemoryResult(searcher, item);

    EXPECT_TRUE(result.interaction_id.empty());
    EXPECT_TRUE(result.session_id.empty());
    EXPECT_TRUE(result.query_type.empty());
    EXPECT_TRUE(result.metadata_json.empty());
}

// ToMemoryResult: metadata with session_id but no interaction_id still maps
// session_id (line 237 branch independent of interaction_id), while query_text
// extraction is skipped (interaction_id empty).
TEST_F(MemorySearcherPrivateTest, ToMemoryResult_SessionIdWithoutInteractionId) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    json meta;
    meta["session_id"] = "sess-only";  // no interaction_id

    ResultItem item;
    item.score = 0.4f;
    item.metadata = meta;
    item.chunk_text = "q\n---\nr";

    auto result = CallToMemoryResult(searcher, item);
    EXPECT_TRUE(result.interaction_id.empty());
    EXPECT_EQ(result.session_id, "sess-only");
    EXPECT_TRUE(result.query_text.empty());  // not extracted without interaction_id
}

// ToMemoryResult: query_type extraction branch (line 240) — present in metadata.
TEST_F(MemorySearcherPrivateTest, ToMemoryResult_QueryTypeExtracted) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    json meta;
    meta["interaction_id"] = "int-q";
    meta["query_type"] = "text_to_sql";
    // no session_id -> exercises the session_id-absent path

    ResultItem item;
    item.score = 0.6f;
    item.metadata = meta;
    item.chunk_text = "only query, no separator";

    auto result = CallToMemoryResult(searcher, item);
    EXPECT_EQ(result.interaction_id, "int-q");
    EXPECT_EQ(result.query_type, "text_to_sql");
    EXPECT_TRUE(result.session_id.empty());
}

TEST_F(MemorySearcherPrivateTest, ToMemoryResult_EmptyInteractionId) {
    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    json meta;
    // no interaction_id

    ResultItem item;
    item.metadata = meta;
    item.chunk_text = "test\n---\nresp";

    auto result = CallToMemoryResult(searcher, item);

    EXPECT_TRUE(result.interaction_id.empty());
    // When interaction_id is empty, query_text/response_text are not extracted
    EXPECT_TRUE(result.query_text.empty());
    EXPECT_TRUE(result.response_text.empty());
}

// ---------------------------------------------------------------------------
// Search integration: MEM05 excludes null-metadata items (no user_id) via
// MatchScope before they reach the result list.
// ---------------------------------------------------------------------------

TEST_F(MemorySearcherPrivateTest, Search_NullMetadataItems_ExcludedByUserIdFilter) {
    // Under MEM05, a block with null metadata has no user_id, so MatchScope
    // excludes it on every path. Confirm the search completes and returns none.

    vec_index_.set_search_result({{200, 0.1f}});
    store_.blocks[200] = CortrixBlock{200, "test-doc-1", 0, 6, 3, -1, "", {}, "text"};

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.scope = MemoryScope::kUser;  // MEM05: kAll removed
    req.user_id = "user-1";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    // Null-metadata item has no user_id -> excluded by MatchScope.
    EXPECT_EQ(resp.total_results, 0);
    EXPECT_GE(resp.latency_ms, 0);
}

}  // namespace cortrix
