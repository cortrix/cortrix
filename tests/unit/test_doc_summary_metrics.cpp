#include <gtest/gtest.h>

#include <string>

#include "cortrix/doc_summary/doc_summary_metrics.h"

// Doc summary S6 — Document Summary metrics (§12, subsystems `doc_summary` +
// `fts5_fallback`). Self-contained recorder + OpenMetrics renderer (same pattern
// as HypeMetrics / SparseMetrics). NO high-cardinality labels (§12).
namespace cortrix::doc_summary {
namespace {

using LlmCallStatus = DocSummaryMetrics::LlmCallStatus;
using FallbackResult = DocSummaryMetrics::FallbackResult;

class DocSummaryMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { DocSummaryMetrics::Instance().ResetForTest(); }
    void TearDown() override { DocSummaryMetrics::Instance().ResetForTest(); }
    DocSummaryMetrics& m() { return DocSummaryMetrics::Instance(); }
};

TEST_F(DocSummaryMetricsTest, LlmCallsCountPerStatus) {
    m().RecordLlmCall(LlmCallStatus::kStarted);
    m().RecordLlmCall(LlmCallStatus::kStarted);
    m().RecordLlmCall(LlmCallStatus::kSuccess);
    m().RecordLlmCall(LlmCallStatus::kFailed);
    EXPECT_EQ(m().LlmCallCount(LlmCallStatus::kStarted), 2u);
    EXPECT_EQ(m().LlmCallCount(LlmCallStatus::kSuccess), 1u);
    EXPECT_EQ(m().LlmCallCount(LlmCallStatus::kFailed), 1u);
}

TEST_F(DocSummaryMetricsTest, LlmDurationHistogramPerModel) {
    m().ObserveLlmDuration("gpt-4o-mini", 0.3);
    m().ObserveLlmDuration("gpt-4o-mini", 1.5);
    EXPECT_EQ(m().LlmDurationCount("gpt-4o-mini"), 2u);
    EXPECT_NEAR(m().LlmDurationSum("gpt-4o-mini"), 1.8, 1e-6);
    EXPECT_EQ(m().LlmDurationCount("unknown"), 0u);
}

TEST_F(DocSummaryMetricsTest, SummariesGeneratedCounter) {
    m().AddSummariesGenerated(1);
    m().AddSummariesGenerated(2);
    EXPECT_EQ(m().SummariesGeneratedCount(), 3u);
}

TEST_F(DocSummaryMetricsTest, FallbackTriggeredPerResult) {
    m().RecordFallbackTriggered(FallbackResult::kHit);
    m().RecordFallbackTriggered(FallbackResult::kHit);
    m().RecordFallbackTriggered(FallbackResult::kMiss);
    EXPECT_EQ(m().FallbackTriggeredCount(FallbackResult::kHit), 2u);
    EXPECT_EQ(m().FallbackTriggeredCount(FallbackResult::kMiss), 1u);
}

TEST_F(DocSummaryMetricsTest, Fts5FallbackFailedCounter) {
    m().RecordFts5FallbackFailed();
    m().RecordFts5FallbackFailed();
    EXPECT_EQ(m().Fts5FallbackFailedCount(), 2u);
}

TEST_F(DocSummaryMetricsTest, LabelStrings) {
    EXPECT_STREQ(ToString(LlmCallStatus::kStarted), "started");
    EXPECT_STREQ(ToString(LlmCallStatus::kSuccess), "success");
    EXPECT_STREQ(ToString(LlmCallStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(FallbackResult::kHit), "hit");
    EXPECT_STREQ(ToString(FallbackResult::kMiss), "miss");
}

TEST_F(DocSummaryMetricsTest, RenderOpenMetricsContainsAllSeries) {
    m().RecordLlmCall(LlmCallStatus::kSuccess);
    m().ObserveLlmDuration("gpt-4o-mini", 0.5);
    m().AddSummariesGenerated(1);
    m().RecordFallbackTriggered(FallbackResult::kHit);
    m().RecordFts5FallbackFailed();
    std::string out = m().RenderOpenMetrics();

    EXPECT_NE(out.find("# TYPE cortrix_doc_summary_llm_calls_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_doc_summary_llm_calls_total{status=\"success\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_doc_summary_summaries_generated_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_doc_summary_summaries_generated_total 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_doc_summary_fallback_triggered_total{result=\"hit\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_fts5_fallback_failed_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_fts5_fallback_failed_total 1"), std::string::npos);
    EXPECT_NE(out.find("cortrix_doc_summary_llm_duration_seconds_bucket{model=\"gpt-4o-mini\""),
              std::string::npos);
}

TEST_F(DocSummaryMetricsTest, RenderHasNoHighCardinalityLabels) {
    m().AddSummariesGenerated(1);
    std::string out = m().RenderOpenMetrics();
    EXPECT_EQ(out.find("doc_id"), std::string::npos);
    EXPECT_EQ(out.find("ns_id"), std::string::npos);
    EXPECT_EQ(out.find("tenant_id"), std::string::npos);
}

TEST_F(DocSummaryMetricsTest, ResetClearsEverything) {
    m().RecordLlmCall(LlmCallStatus::kSuccess);
    m().ObserveLlmDuration("m", 1.0);
    m().AddSummariesGenerated(5);
    m().RecordFallbackTriggered(FallbackResult::kHit);
    m().RecordFts5FallbackFailed();
    m().ResetForTest();
    EXPECT_EQ(m().LlmCallCount(LlmCallStatus::kSuccess), 0u);
    EXPECT_EQ(m().LlmDurationCount("m"), 0u);
    EXPECT_EQ(m().SummariesGeneratedCount(), 0u);
    EXPECT_EQ(m().FallbackTriggeredCount(FallbackResult::kHit), 0u);
    EXPECT_EQ(m().Fts5FallbackFailedCount(), 0u);
}

}  // namespace
}  // namespace cortrix::doc_summary
