#include <gtest/gtest.h>

#include <string>

#include "cortrix/spc/hype_metrics.h"

// HyPE S7 — the 4 hype_index subsystem metrics (§11). Recorder + OpenMetrics
// render + no-high-cardinality-label compliance.
namespace cortrix::spc {
namespace {

using LS = HypeMetrics::LlmCallStatus;
using HT = HypeMetrics::MatchHitType;

class HypeMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { HypeMetrics::Instance().ResetForTest(); }
    void TearDown() override { HypeMetrics::Instance().ResetForTest(); }
    HypeMetrics& M() { return HypeMetrics::Instance(); }
};

TEST_F(HypeMetricsTest, LlmCallsCounterPerStatus) {
    M().RecordLlmCall(LS::kSuccess);
    M().RecordLlmCall(LS::kSuccess);
    M().RecordLlmCall(LS::kFailed);
    EXPECT_EQ(M().LlmCallCount(LS::kSuccess), 2u);
    EXPECT_EQ(M().LlmCallCount(LS::kFailed), 1u);
}

TEST_F(HypeMetricsTest, QuestionsGeneratedCounter) {
    M().AddQuestionsGenerated(3);
    M().AddQuestionsGenerated(3);
    EXPECT_EQ(M().QuestionsGeneratedCount(), 6u);
}

TEST_F(HypeMetricsTest, MatchCounterPerHitType) {
    M().RecordMatch(HT::kChunk);
    M().RecordMatch(HT::kHype);
    M().RecordMatch(HT::kBoth);
    M().RecordMatch(HT::kBoth);
    EXPECT_EQ(M().MatchCount(HT::kChunk), 1u);
    EXPECT_EQ(M().MatchCount(HT::kHype), 1u);
    EXPECT_EQ(M().MatchCount(HT::kBoth), 2u);
}

TEST_F(HypeMetricsTest, LlmDurationHistogramPerModel) {
    M().ObserveLlmDuration("gpt-4o-mini", 0.3);
    M().ObserveLlmDuration("gpt-4o-mini", 1.5);
    EXPECT_EQ(M().LlmDurationCount("gpt-4o-mini"), 2u);
    EXPECT_DOUBLE_EQ(M().LlmDurationSum("gpt-4o-mini"), 1.8);
    EXPECT_EQ(M().LlmDurationCount("other-model"), 0u);  // per-model isolation
}

TEST_F(HypeMetricsTest, LabelStrings) {
    EXPECT_STREQ(ToString(LS::kSuccess), "success");
    EXPECT_STREQ(ToString(LS::kFailed), "failed");
    EXPECT_STREQ(ToString(HT::kChunk), "chunk");
    EXPECT_STREQ(ToString(HT::kHype), "hype");
    EXPECT_STREQ(ToString(HT::kBoth), "both");
}

TEST_F(HypeMetricsTest, RenderContainsAll4Metrics) {
    M().RecordLlmCall(LS::kSuccess);
    M().AddQuestionsGenerated(3);
    M().RecordMatch(HT::kBoth);
    M().ObserveLlmDuration("gpt-4o-mini", 0.5);
    std::string out = M().RenderOpenMetrics();

    EXPECT_NE(out.find("cortrix_hype_index_llm_calls_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_hype_index_questions_generated_total"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_hype_index_match_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_hype_index_llm_duration_seconds"), std::string::npos);

    // labels present + histogram structure.
    EXPECT_NE(out.find("status=\"success\""), std::string::npos);
    EXPECT_NE(out.find("hit_type=\"both\""), std::string::npos);
    EXPECT_NE(out.find("model=\"gpt-4o-mini\""), std::string::npos);
    EXPECT_NE(out.find("le=\"+Inf\""), std::string::npos);
    EXPECT_NE(out.find("_bucket{"), std::string::npos);
}

TEST_F(HypeMetricsTest, NoForbiddenHighCardinalityLabels) {
    M().RecordLlmCall(LS::kSuccess);
    M().RecordMatch(HT::kChunk);
    M().ObserveLlmDuration("gpt-4o-mini", 0.5);
    std::string out = M().RenderOpenMetrics();
    // §11 / OBS_SPEC §3.2 forbidden labels must NOT appear.
    EXPECT_EQ(out.find("chunk_id"), std::string::npos);
    EXPECT_EQ(out.find("ns_id"), std::string::npos);
    EXPECT_EQ(out.find("tenant"), std::string::npos);
    EXPECT_EQ(out.find("user_id"), std::string::npos);
    EXPECT_EQ(out.find("request_id"), std::string::npos);
}

TEST_F(HypeMetricsTest, ResetForTestClearsAll) {
    M().RecordLlmCall(LS::kSuccess);
    M().AddQuestionsGenerated(5);
    M().RecordMatch(HT::kHype);
    M().ObserveLlmDuration("m", 1.0);
    M().ResetForTest();
    EXPECT_EQ(M().LlmCallCount(LS::kSuccess), 0u);
    EXPECT_EQ(M().QuestionsGeneratedCount(), 0u);
    EXPECT_EQ(M().MatchCount(HT::kHype), 0u);
    EXPECT_EQ(M().LlmDurationCount("m"), 0u);
}

}  // namespace
}  // namespace cortrix::spc
