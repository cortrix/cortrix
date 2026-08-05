#include <gtest/gtest.h>

#include <string>

#include "cortrix/spc/contextual_metrics.h"

// Contextual retrieval S7 — Contextual Retrieval metrics (§10, subsystem `contextual_retrieval`).
// Self-contained recorder + OpenMetrics renderer (same pattern as HypeMetrics
// / SparseMetrics). NO high-cardinality labels (§10 V3 ruling 10 dropped ns_id).
namespace cortrix::spc {
namespace {

using ChunkStatus = ContextualRetrievalMetrics::ChunkStatus;
using QueryPath = ContextualRetrievalMetrics::QueryPath;

class ContextualMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { ContextualRetrievalMetrics::Instance().ResetForTest(); }
    void TearDown() override { ContextualRetrievalMetrics::Instance().ResetForTest(); }
    ContextualRetrievalMetrics& m() { return ContextualRetrievalMetrics::Instance(); }
};

TEST_F(ContextualMetricsTest, ChunksTotalCountsPerStatus) {
    m().RecordChunk(ChunkStatus::kGenerated);
    m().RecordChunk(ChunkStatus::kGenerated);
    m().RecordChunk(ChunkStatus::kFailed);
    m().RecordChunk(ChunkStatus::kSkippedNoLlm);
    EXPECT_EQ(m().ChunkCount(ChunkStatus::kGenerated), 2u);
    EXPECT_EQ(m().ChunkCount(ChunkStatus::kFailed), 1u);
    EXPECT_EQ(m().ChunkCount(ChunkStatus::kSkippedNoLlm), 1u);
}

TEST_F(ContextualMetricsTest, LlmCostTokensPerModel) {
    m().AddLlmCostTokens("gpt-4o-mini", 100);
    m().AddLlmCostTokens("gpt-4o-mini", 50);
    m().AddLlmCostTokens("gpt-4o", 200);
    EXPECT_EQ(m().LlmCostTokens("gpt-4o-mini"), 150);
    EXPECT_EQ(m().LlmCostTokens("gpt-4o"), 200);
    EXPECT_EQ(m().LlmCostTokens("unknown"), 0);
}

TEST_F(ContextualMetricsTest, FallbackRatioGaugeClamps) {
    m().SetFallbackRatio(0.25);
    EXPECT_NEAR(m().FallbackRatio(), 0.25, 1e-5);
    m().SetFallbackRatio(-1.0);  // clamps to 0
    EXPECT_NEAR(m().FallbackRatio(), 0.0, 1e-5);
    m().SetFallbackRatio(2.0);   // clamps to 1
    EXPECT_NEAR(m().FallbackRatio(), 1.0, 1e-5);
}

TEST_F(ContextualMetricsTest, QueryViaPathCountsPerPath) {
    m().RecordQueryPath(QueryPath::kDense);
    m().RecordQueryPath(QueryPath::kContextualized);
    m().RecordQueryPath(QueryPath::kContextualized);
    m().RecordQueryPath(QueryPath::kFallbackDense);
    EXPECT_EQ(m().QueryPathCount(QueryPath::kDense), 1u);
    EXPECT_EQ(m().QueryPathCount(QueryPath::kContextualized), 2u);
    EXPECT_EQ(m().QueryPathCount(QueryPath::kFallbackDense), 1u);
}

TEST_F(ContextualMetricsTest, StatusLabelStrings) {
    EXPECT_STREQ(ToString(ChunkStatus::kGenerated), "generated");
    EXPECT_STREQ(ToString(ChunkStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(ChunkStatus::kSkippedNoLlm), "skipped_no_llm");
}

TEST_F(ContextualMetricsTest, PathLabelStrings) {
    EXPECT_STREQ(ToString(QueryPath::kDense), "dense");
    EXPECT_STREQ(ToString(QueryPath::kContextualized), "contextualized");
    EXPECT_STREQ(ToString(QueryPath::kFallbackDense), "fallback_dense");
}

TEST_F(ContextualMetricsTest, RenderOpenMetricsContainsAllSeries) {
    m().RecordChunk(ChunkStatus::kGenerated);
    m().AddLlmCostTokens("gpt-4o-mini", 80);
    m().SetFallbackRatio(0.1);
    m().RecordQueryPath(QueryPath::kContextualized);
    std::string out = m().RenderOpenMetrics();

    EXPECT_NE(out.find("# TYPE cortrix_contextual_retrieval_chunks_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_contextual_retrieval_chunks_total{status=\"generated\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_contextual_retrieval_llm_cost_tokens counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_contextual_retrieval_llm_cost_tokens{model=\"gpt-4o-mini\"} 80"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_contextual_retrieval_fallback_ratio gauge"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_contextual_retrieval_query_via_path_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_contextual_retrieval_query_via_path_total{path=\"contextualized\"} 1"),
              std::string::npos);
}

TEST_F(ContextualMetricsTest, RenderHasNoHighCardinalityLabels) {
    // §10 V3 ruling 10: ns_id (and other high-card labels) must NOT appear.
    m().RecordChunk(ChunkStatus::kGenerated);
    std::string out = m().RenderOpenMetrics();
    EXPECT_EQ(out.find("ns_id"), std::string::npos);
    EXPECT_EQ(out.find("tenant_id"), std::string::npos);
    EXPECT_EQ(out.find("chunk_id"), std::string::npos);
}

TEST_F(ContextualMetricsTest, ResetClearsEverything) {
    m().RecordChunk(ChunkStatus::kGenerated);
    m().AddLlmCostTokens("gpt-4o-mini", 80);
    m().SetFallbackRatio(0.5);
    m().RecordQueryPath(QueryPath::kDense);
    m().ResetForTest();
    EXPECT_EQ(m().ChunkCount(ChunkStatus::kGenerated), 0u);
    EXPECT_EQ(m().LlmCostTokens("gpt-4o-mini"), 0);
    EXPECT_NEAR(m().FallbackRatio(), 0.0, 1e-5);
    EXPECT_EQ(m().QueryPathCount(QueryPath::kDense), 0u);
}

}  // namespace
}  // namespace cortrix::spc
