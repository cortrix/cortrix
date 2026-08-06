#include <gtest/gtest.h>

#include <string>

#include "cortrix/memory/memory_extract_metrics.h"

// S9 coverage: the 6 cortrix_memory_extract_* metrics — counters/gauge/histogram
// recording + the OpenMetrics renderer + label-enum discipline (no
// high-cardinality labels).
namespace cortrix::memory {
namespace {

using ExtractStatus = MemoryExtractMetrics::ExtractStatus;
using TokenDirection = MemoryExtractMetrics::TokenDirection;
using ConfidenceBucket = MemoryExtractMetrics::ConfidenceBucket;
using TriggeredBy = MemoryExtractMetrics::TriggeredBy;

class MemoryExtractMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { MemoryExtractMetrics::Instance().ResetForTest(); }
    void TearDown() override { MemoryExtractMetrics::Instance().ResetForTest(); }
    MemoryExtractMetrics& M() { return MemoryExtractMetrics::Instance(); }
};

TEST_F(MemoryExtractMetricsTest, ExtractTotalCountsByStatus) {
    M().RecordExtract(ExtractStatus::kSuccess);
    M().RecordExtract(ExtractStatus::kSuccess);
    M().RecordExtract(ExtractStatus::kFailed);
    EXPECT_EQ(M().ExtractCount(ExtractStatus::kSuccess), 2u);
    EXPECT_EQ(M().ExtractCount(ExtractStatus::kFailed), 1u);
}

TEST_F(MemoryExtractMetricsTest, TokensSummedByDirectionAcrossModels) {
    M().RecordTokens("gpt-4o-mini", TokenDirection::kInput, 100);
    M().RecordTokens("gpt-4o-mini", TokenDirection::kOutput, 40);
    M().RecordTokens("claude-haiku-4-5", TokenDirection::kInput, 50);
    EXPECT_EQ(M().TokenCount(TokenDirection::kInput), 150u);
    EXPECT_EQ(M().TokenCount(TokenDirection::kOutput), 40u);
}

TEST_F(MemoryExtractMetricsTest, QueueDepthGaugeSetAndClamped) {
    M().SetQueueDepth(7);
    EXPECT_EQ(M().QueueDepth(), 7);
    M().SetQueueDepth(-3);  // clamped to 0
    EXPECT_EQ(M().QueueDepth(), 0);
}

TEST_F(MemoryExtractMetricsTest, ContradictionBucketing) {
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.95), ConfidenceBucket::kHigh);
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.6), ConfidenceBucket::kMedium);
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.2), ConfidenceBucket::kLow);
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.8), ConfidenceBucket::kHigh);   // boundary
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.5), ConfidenceBucket::kMedium); // boundary

    M().RecordContradiction(ConfidenceBucket::kHigh);
    M().RecordContradiction(ConfidenceBucket::kLow);
    M().RecordContradiction(ConfidenceBucket::kLow);
    EXPECT_EQ(M().ContradictionCount(ConfidenceBucket::kHigh), 1u);
    EXPECT_EQ(M().ContradictionCount(ConfidenceBucket::kLow), 2u);
}

TEST_F(MemoryExtractMetricsTest, InvalidationsByTrigger) {
    M().RecordInvalidation(TriggeredBy::kLlmAuto);
    M().RecordInvalidation(TriggeredBy::kAgentSelf);
    M().RecordInvalidation(TriggeredBy::kLlmAuto);
    EXPECT_EQ(M().InvalidationCount(TriggeredBy::kLlmAuto), 2u);
    EXPECT_EQ(M().InvalidationCount(TriggeredBy::kAgentSelf), 1u);
    EXPECT_EQ(M().InvalidationCount(TriggeredBy::kManual), 0u);
}

TEST_F(MemoryExtractMetricsTest, RenderOpenMetricsHasAllSixMetricsAndTypes) {
    M().RecordExtract(ExtractStatus::kSuccess);
    M().ObserveExtractDuration("gpt-4o-mini", 320);
    M().SetQueueDepth(3);
    M().RecordTokens("gpt-4o-mini", TokenDirection::kInput, 100);
    M().RecordContradiction(ConfidenceBucket::kMedium);
    M().RecordInvalidation(TriggeredBy::kLlmAuto);

    std::string out = M().RenderOpenMetrics();
    // All 6 metric names present.
    EXPECT_NE(out.find("cortrix_memory_extract_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_extract_duration_seconds"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_extract_queue_depth"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_extract_llm_tokens_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_extract_contradictions_found_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_extract_invalidations_total"), std::string::npos);
    // TYPE lines present.
    EXPECT_NE(out.find("# TYPE cortrix_memory_extract_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_extract_queue_depth gauge"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_extract_duration_seconds histogram"),
              std::string::npos);
    // A histogram MUST render cumulative _bucket{le=...} lines incl +Inf (not just
    // _sum/_count) to be valid OpenMetrics.
    EXPECT_NE(out.find("cortrix_memory_extract_duration_seconds_bucket{"), std::string::npos);
    EXPECT_NE(out.find("le=\"+Inf\""), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_extract_duration_seconds_sum{"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_extract_duration_seconds_count{"), std::string::npos);
}

// The duration histogram renders well-formed cumulative buckets: every declared
// le bound + a +Inf bucket, monotonically non-decreasing, with +Inf == _count.
TEST_F(MemoryExtractMetricsTest, ExtractDurationHistogramBucketsAreCumulativeAndComplete) {
    // Observations spanning several buckets for one model (ms): 50→0.05s (le 0.1),
    // 300→0.3s (le 0.5), 1500→1.5s (le 2), 40000→40s (+Inf only).
    M().ObserveExtractDuration("gpt-4o-mini", 50);
    M().ObserveExtractDuration("gpt-4o-mini", 300);
    M().ObserveExtractDuration("gpt-4o-mini", 1500);
    M().ObserveExtractDuration("gpt-4o-mini", 40000);
    std::string out = M().RenderOpenMetrics();

    const std::string prefix =
        "cortrix_memory_extract_duration_seconds_bucket{model=\"gpt-4o-mini\",le=\"";
    auto bucket_value = [&](const std::string& le) -> long {
        std::string key = prefix + le + "\"} ";
        size_t p = out.find(key);
        if (p == std::string::npos) return -1;
        p += key.size();
        return std::stol(out.substr(p, out.find('\n', p) - p));
    };

    // All eight declared bounds + +Inf appear as bucket lines for the model.
    long prev = 0;
    for (const char* le : {"0.1", "0.25", "0.5", "1", "2", "5", "10", "30"}) {
        long v = bucket_value(le);
        ASSERT_GE(v, 0) << "missing bucket le=" << le;
        EXPECT_GE(v, prev) << "bucket le=" << le << " not cumulative";  // monotonic
        prev = v;
    }
    EXPECT_EQ(bucket_value("0.1"), 1);   // 0.05s
    EXPECT_EQ(bucket_value("0.5"), 2);   // 0.05 + 0.3
    EXPECT_EQ(bucket_value("2"), 3);     // + 1.5
    EXPECT_EQ(bucket_value("30"), 3);    // 40s only in +Inf
    EXPECT_EQ(bucket_value("+Inf"), 4);  // +Inf == total observations
    EXPECT_NE(out.find("cortrix_memory_extract_duration_seconds_count{model=\"gpt-4o-mini\"} 4"),
              std::string::npos);
}

TEST_F(MemoryExtractMetricsTest, RenderHasNoHighCardinalityLabels) {
    // OBS_SPEC: labels are enum-only or low-cardinality model strings.
    // No tenant_id / ns_id / user_id labels ever appear.
    M().RecordExtract(ExtractStatus::kSuccess);
    M().RecordTokens("gpt-4o-mini", TokenDirection::kInput, 10);
    std::string out = M().RenderOpenMetrics();
    EXPECT_EQ(out.find("tenant_id"), std::string::npos);
    EXPECT_EQ(out.find("ns_id"), std::string::npos);
    EXPECT_EQ(out.find("user_id"), std::string::npos);
}

TEST_F(MemoryExtractMetricsTest, ModelOverflowFoldsIntoOtherBucket) {
    // More distinct models than table slots (8, last is overflow) fold into "other".
    for (int i = 0; i < 12; ++i) {
        M().ObserveExtractDuration("model_" + std::to_string(i), 10);
    }
    std::string out = M().RenderOpenMetrics();
    EXPECT_NE(out.find("model=\"other\""), std::string::npos);
}

TEST_F(MemoryExtractMetricsTest, LabelStringsMatchSpec) {
    EXPECT_STREQ(ToString(ExtractStatus::kSuccess), "success");
    EXPECT_STREQ(ToString(ExtractStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(TokenDirection::kInput), "input");
    EXPECT_STREQ(ToString(TokenDirection::kOutput), "output");
    EXPECT_STREQ(ToString(ConfidenceBucket::kHigh), "high");
    EXPECT_STREQ(ToString(ConfidenceBucket::kMedium), "medium");
    EXPECT_STREQ(ToString(ConfidenceBucket::kLow), "low");
    EXPECT_STREQ(ToString(TriggeredBy::kLlmAuto), "llm_auto");
    EXPECT_STREQ(ToString(TriggeredBy::kManual), "manual");
    EXPECT_STREQ(ToString(TriggeredBy::kAgentSelf), "agent_self");
}

}  // namespace
}  // namespace cortrix::memory
