// test_query_coverage_gaps.cpp — targeted unit coverage for the residual core-17
// QUERY line-gate gaps (the 90% core-17 line gate). Each case below pins a specific
// uncovered arm identified from the lcov core17 report, NOT already exercised by the
// existing query tests (test_post_filter / test_rag_fusion / test_vector_searcher /
// test_wordpiece_unicode_r8 / test_onnx_complexity_backend).
//
// Deterministic only: a fake in-process CortrixStore + the frozen MockLlmClient +
// the stub OnnxEmbedder + a mock IIndex. No live LLM / model / network / sleeps
// beyond a single bounded 2ms latency probe.
//
// gtest suite names are GLOBAL — every fixture/suite here is module-prefixed
// "QueryCovGap*" so it never clashes with the existing PostFilterTest / RagFusionTest
// / VectorSearcherRealTest suites.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/common/data_types.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/rag_fusion.h"
#include "cortrix/query/rag_fusion_metrics.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/scored_block.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/iindex.h"

#include "mocks/mock_llm_client.h"

// ===========================================================================
// PostFilter: block-metadata merge / catch arms + Apply exclude_types +
// soft-deleted-doc skips (post_filter.cpp lines 44-46, 78-87, 117-123, 158,
// 187-189). The existing test_post_filter only sets DOC metadata + tests
// exclude_types through FetchByDocId, so these arms stay uncovered.
// ===========================================================================
namespace cortrix {
namespace {

class QueryCovGapFakeStore : public CortrixStore {
public:
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
    int block_get_by_doc(const std::string& doc_id, std::vector<CortrixBlock>& out) override {
        for (const auto& [bid, blk] : blocks) {
            if (blk.doc_id == doc_id) out.push_back(blk);
        }
        std::sort(out.begin(), out.end(),
                  [](const CortrixBlock& a, const CortrixBlock& b) {
                      return a.chunk_index < b.chunk_index;
                  });
        return 0;
    }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t*) override { return 0; }

    int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return 0; }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

class QueryCovGapPostFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        CortrixDoc doc;
        doc.doc_id = "qcg-doc-1";
        doc.source_path = "/contracts/2024/risk.pdf";
        doc.created_at = "2024-06-01T00:00:00Z";
        doc.metadata_json = R"({"user_id":"doc-level","scope":"doc"})";
        store_.docs["qcg-doc-1"] = doc;

        CortrixBlock blk;
        blk.block_id = 11;
        blk.doc_id = "qcg-doc-1";
        blk.block_type = 1;  // FILE
        blk.content_text = "block content";
        store_.blocks[11] = blk;

        filter_ = std::make_unique<PostFilter>(store_);
    }

    std::vector<ScoredBlock> One(int64_t id) {
        ScoredBlock sb;
        sb.block_id = id;
        sb.rrf_score = 0.5f;
        sb.hit_routes = kRouteVector;
        return {sb};
    }

    QueryCovGapFakeStore store_;
    std::unique_ptr<PostFilter> filter_;
};

// Block-level metadata is a VALID object → merged over the doc metadata, block keys
// winning (post_filter.cpp 78-83). user_id from the block overrides the doc's.
TEST_F(QueryCovGapPostFilterTest, Apply_BlockMetadataOverridesDoc) {
    store_.blocks[11].metadata_json = R"({"user_id":"block-level","memory_type":"fact"})";
    QueryFilter f;
    auto results = filter_->Apply(One(11), f, 10);
    ASSERT_EQ(results.size(), 1u);
    // block key wins over the doc key
    EXPECT_EQ(results[0].metadata["user_id"], "block-level");
    // block-only key is present
    EXPECT_EQ(results[0].metadata["memory_type"], "fact");
    // doc-only key survives (merge, not replace)
    EXPECT_EQ(results[0].metadata["scope"], "doc");
}

// Block-level metadata is INVALID JSON → the catch arm keeps the doc metadata only
// (post_filter.cpp 84-86).
TEST_F(QueryCovGapPostFilterTest, Apply_InvalidBlockMetadataFallsBackToDoc) {
    store_.blocks[11].metadata_json = "{not valid json";
    QueryFilter f;
    auto results = filter_->Apply(One(11), f, 10);
    ASSERT_EQ(results.size(), 1u);
    // doc metadata preserved; the bad block JSON was ignored
    EXPECT_EQ(results[0].metadata["user_id"], "doc-level");
    EXPECT_FALSE(results[0].metadata.contains("memory_type"));
}

// Block-level metadata is a valid JSON SCALAR (not an object) → the is_object()
// false arm: merge skipped, doc metadata retained (post_filter.cpp 79 false).
TEST_F(QueryCovGapPostFilterTest, Apply_NonObjectBlockMetadataIgnored) {
    store_.blocks[11].metadata_json = R"("just a string")";
    QueryFilter f;
    auto results = filter_->Apply(One(11), f, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].metadata["user_id"], "doc-level");
}

// MatchFilter exclude_types EXCLUDES the block in the Apply() path
// (post_filter.cpp 117-122 — only FetchByDocId exercised this before).
TEST_F(QueryCovGapPostFilterTest, Apply_ExcludeTypesRemovesMatch) {
    QueryFilter f;
    f.exclude_types = {"FILE"};  // block 11 is FILE → excluded
    auto results = filter_->Apply(One(11), f, 10);
    EXPECT_TRUE(results.empty());
}

// MatchFilter exclude_types present but does NOT match this block's type → the
// for-loop completes without returning false; the block passes (117-123 true→fall).
TEST_F(QueryCovGapPostFilterTest, Apply_ExcludeTypesNoMatchKept) {
    QueryFilter f;
    f.exclude_types = {"DATABASE"};  // block 11 is FILE → not excluded
    auto results = filter_->Apply(One(11), f, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 11);
}

// Apply: a soft-deleted (kDeleted) doc's blocks are invisible to retrieval
// (post_filter.cpp 44-46).
TEST_F(QueryCovGapPostFilterTest, Apply_SoftDeletedDocSkipped) {
    store_.docs["qcg-doc-1"].status = DocStatus::kDeleted;
    QueryFilter f;
    auto results = filter_->Apply(One(11), f, 10);
    EXPECT_TRUE(results.empty());
}

// FetchByDocId: a soft-deleted doc returns no rows (post_filter.cpp 156-158).
TEST_F(QueryCovGapPostFilterTest, FetchByDocId_SoftDeletedDocReturnsEmpty) {
    store_.docs["qcg-doc-1"].status = DocStatus::kDeleted;
    QueryFilter f;
    auto results = filter_->FetchByDocId("qcg-doc-1", f, 10);
    EXPECT_TRUE(results.empty());
}

// FetchByDocId: doc metadata is VALID JSON → parsed into item.metadata
// (post_filter.cpp 186-187 success arm).
TEST_F(QueryCovGapPostFilterTest, FetchByDocId_ValidDocMetadataParsed) {
    QueryFilter f;
    auto results = filter_->FetchByDocId("qcg-doc-1", f, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].metadata["user_id"], "doc-level");
}

// FetchByDocId: doc metadata is INVALID JSON → the catch arm leaves an empty object
// (post_filter.cpp 188-189 catch).
TEST_F(QueryCovGapPostFilterTest, FetchByDocId_InvalidDocMetadataEmptyObject) {
    store_.docs["qcg-doc-1"].metadata_json = "{still not json";
    QueryFilter f;
    auto results = filter_->FetchByDocId("qcg-doc-1", f, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].metadata.is_object());
    EXPECT_TRUE(results[0].metadata.empty());
}

}  // namespace
}  // namespace cortrix

// ===========================================================================
// RagFusion service-level degrade-reason tokens for circuit/quota/invalid, plus
// the ObserveLlmLatency model-name path (rag_fusion.cpp DegradeReasonToken 22-25 +
// 113-116). The existing test_rag_fusion only checks the "llm_timeout" service
// degrade reason; the other three DegradeReasonToken arms are only hit at the
// generator level there, not via RagFusion::ExpandQueries' ExplainState.
// ===========================================================================
namespace cortrix::query {
namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;
using llm::ChatCompletionResponse;
using llm::MockLlmClient;

ChatCompletionResponse QcgOk(const std::string& content,
                             const std::string& model = "gpt-4o-mini") {
    ChatCompletionResponse r;
    r.status = Status::Ok();
    r.content = content;
    r.model = model;
    r.finish_reason = "stop";
    r.prompt_tokens = 11;
    r.completion_tokens = 22;
    return r;
}

ChatCompletionResponse QcgErr(StatusCode code, const std::string& msg) {
    ChatCompletionResponse r;
    r.status = Status(code, msg);
    return r;
}

std::string QcgThreeVariants() {
    return R"({"variants":[
        {"strategy":"paraphrase","query":"alpha variant"},
        {"strategy":"subquery","query":"beta variant"},
        {"strategy":"reverse","query":"gamma variant"}
    ]})";
}

RagFusionConfig QcgEnabled() {
    RagFusionConfig c;
    c.enabled = true;
    c.variant_count = 3;
    return c;
}

std::shared_ptr<RagFusion> QcgService(std::shared_ptr<MockLlmClient> mock) {
    auto gen = std::make_shared<QueryVariantGenerator>(mock);
    auto rrf = std::make_shared<RRFFusion>(60);
    return std::make_shared<RagFusion>(gen, rrf);
}

class QueryCovGapRagFusionTest : public ::testing::Test {
protected:
    void SetUp() override { RagFusionMetrics::Instance().ResetForTest(); }
};

// DegradeReasonToken "circuit_open" arm via ExpandQueries (rag_fusion.cpp 23).
TEST_F(QueryCovGapRagFusionTest, ExpandQueries_DegradeReason_CircuitOpen) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(QcgErr(StatusCode::kUnavailable, "circuit breaker open")));
    auto svc = QcgService(mock);
    auto out = svc->ExpandQueries("q", QcgEnabled());
    ASSERT_FALSE(out.ok());
    auto es = svc->GetExplainState();
    EXPECT_TRUE(es.degraded);
    EXPECT_EQ(es.degrade_reason, "circuit_open");
}

// DegradeReasonToken "quota_exceeded" arm (rag_fusion.cpp 24).
TEST_F(QueryCovGapRagFusionTest, ExpandQueries_DegradeReason_QuotaExceeded) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(QcgErr(StatusCode::kPermissionDenied, "quota exceeded (429)")));
    auto svc = QcgService(mock);
    auto out = svc->ExpandQueries("q", QcgEnabled());
    ASSERT_FALSE(out.ok());
    EXPECT_EQ(svc->GetExplainState().degrade_reason, "quota_exceeded");
}

// DegradeReasonToken "invalid_response" arm (rag_fusion.cpp 25). The generator maps
// an unparseable LLM body to CX_ERR_RAG_FUSION_INVALID_RESPONSE; ExpandQueries then
// resolves the token to "invalid_response".
TEST_F(QueryCovGapRagFusionTest, ExpandQueries_DegradeReason_InvalidResponse) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(QcgOk("definitely not json")));  // OK status, bad content
    auto svc = QcgService(mock);
    auto out = svc->ExpandQueries("q", QcgEnabled());
    ASSERT_FALSE(out.ok());
    EXPECT_EQ(svc->GetExplainState().degrade_reason, "invalid_response");
}

// Nested generic LLM transport token should not be masked as llm_timeout by the
// F36 wrapper. This is critical for benchmark explainability: "network/TLS" and
// "deadline exceeded" require different follow-up actions.
TEST_F(QueryCovGapRagFusionTest, ExpandQueries_DegradeReason_LlmTransport) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(QcgErr(StatusCode::kUnavailable,
                                "CX_LLM_TRANSPORT: transport failure to endpoint")));
    auto svc = QcgService(mock);
    auto out = svc->ExpandQueries("q", QcgEnabled());
    ASSERT_FALSE(out.ok());
    EXPECT_EQ(svc->GetExplainState().degrade_reason, "llm_transport");
    EXPECT_EQ(svc->GetExplainState().degrade_detail, "transport_failure");
    EXPECT_EQ(RagFusionMetrics::Instance().DegradedCount(
                  RagFusionMetrics::DegradeReason::kLlmTransport), 1u);
}

// Non-429 HTTP failures from the shared LLM client stay a degraded fallback, but
// explain/metrics must preserve that they were HTTP/auth/provider errors rather
// than true timeouts.
TEST_F(QueryCovGapRagFusionTest, ExpandQueries_DegradeReason_LlmHttp) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(QcgErr(StatusCode::kInternal,
                                "CX_LLM_HTTP: http_status=401")));
    auto svc = QcgService(mock);
    auto out = svc->ExpandQueries("q", QcgEnabled());
    ASSERT_FALSE(out.ok());
    EXPECT_EQ(svc->GetExplainState().degrade_reason, "llm_http");
    EXPECT_EQ(svc->GetExplainState().degrade_detail, "http_status=401");
    EXPECT_EQ(RagFusionMetrics::Instance().DegradedCount(
                  RagFusionMetrics::DegradeReason::kLlmHttp), 1u);
}

// ObserveLlmLatency with a non-empty model name on the success path
// (rag_fusion.cpp 113-116). The mock's Chat sleeps a bounded 2ms so the measured
// llm_latency_ms is > 0 (the gate on line 113), and the response carries a model
// name ("gpt-4o-mini") so the empty-name "unknown" fallback is NOT taken.
TEST_F(QueryCovGapRagFusionTest, ExpandQueries_Success_ObservesLlmLatencyWithModel) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _)).WillOnce(Invoke(
        [](const std::string&, const llm::LlmCallConfig&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return QcgOk(QcgThreeVariants(), "gpt-4o-mini");
        }));
    auto svc = QcgService(mock);
    auto out = svc->ExpandQueries("company status", QcgEnabled());
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().size(), 4u);
    auto es = svc->GetExplainState();
    ASSERT_TRUE(es.llm_latency_ms.has_value());
    EXPECT_GT(*es.llm_latency_ms, 0);  // proves the latency_ms>0 branch fired
    // The named-model histogram label is observable in the rendered metrics.
    std::string text = RagFusionMetrics::Instance().RenderOpenMetrics();
    EXPECT_NE(text.find("gpt-4o-mini"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::query

// ===========================================================================
// VectorSearcher: the timeout-after-embedding arm (vector_searcher.cpp 31-36).
// A non-positive timeout_us makes the post-embedding elapsed exceed the budget, so
// the searcher returns kTimeout WITHOUT ever querying the index.
// ===========================================================================
namespace cortrix {
namespace {

using ::testing::_;

class QueryCovGapMockIIndex : public cortrix::store::IIndex {
public:
    MOCK_METHOD(Status, AddPoint,
                (const float*, uint64_t, const cortrix::observability::TraceContext*), (override));
    MOCK_METHOD(Status, AddPoints,
                ((const std::vector<std::pair<const float*, uint64_t>>&),
                 const cortrix::observability::TraceContext*), (override));
    MOCK_METHOD(Status, MarkDelete,
                (uint64_t, const cortrix::observability::TraceContext*), (override));
    MOCK_METHOD((std::vector<std::pair<uint64_t, float>>), Search,
                (const float*, int, int, const cortrix::observability::TraceContext*), (override));
    MOCK_METHOD(bool, Exists, (uint64_t), (override));
    MOCK_METHOD(Status, Snapshot, (), (override));
    MOCK_METHOD(Status, Recover, (), (override));
    MOCK_METHOD(Status, Shutdown, (), (override));
    MOCK_METHOD(cortrix::store::IndexStats, GetStats, (), (override));
    MOCK_METHOD(std::size_t, GetMemoryFootprintBytes, (), (const, override));
};

TEST(QueryCovGapVectorSearcherTest, TimeoutAfterEmbedding_ReturnsTimeoutNoIndexQuery) {
    OnnxEmbedder embedder("", 128);
    ASSERT_TRUE(embedder.Init().ok());

    QueryCovGapMockIIndex index;
    // timeout already blown → Search must NOT be called. A negative budget makes the
    // post-embedding elapsed (>= 0) strictly exceed it regardless of clock resolution.
    EXPECT_CALL(index, Search(_, _, _, _)).Times(0);

    VectorSearcher searcher(index, embedder);
    auto result = searcher.Search("anything", 10, /*timeout_us=*/-1);

    EXPECT_EQ(result.route_name, "vector");
    EXPECT_EQ(result.status, RouteStatus::kTimeout);
    EXPECT_NE(result.error_message.find("timeout after embedding"), std::string::npos);
    EXPECT_TRUE(result.items.empty());
}

}  // namespace
}  // namespace cortrix
