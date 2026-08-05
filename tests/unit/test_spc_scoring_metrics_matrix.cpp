// SPC + scoring + ONNX metrics — exhaustive matrix coverage for HypeMetrics,
// ContextualRetrievalMetrics, EnricherMetrics, OnnxMetrics, ScoringMetrics
// (enum ToString, label-combo counters, gauges, histograms, render, reset).
// Separate suite namespaces from the existing per-feature metric tests.
#include <gtest/gtest.h>

#include <array>
#include <string>

#include "cortrix/onnx/onnx_metrics.h"
#include "cortrix/scoring/scoring_error.h"
#include "cortrix/scoring/scoring_metrics.h"
#include "cortrix/spc/contextual_metrics.h"
#include "cortrix/spc/hype_metrics.h"
#include "cortrix/spc_enricher/enricher_error.h"
#include "cortrix/spc_enricher/enricher_metrics.h"

// ============================ Hype =====================================
namespace cortrix::spc {
namespace hype_matrix {

using LS = HypeMetrics::LlmCallStatus;
using HT = HypeMetrics::MatchHitType;

class HypeFx : public ::testing::Test {
protected:
    void SetUp() override { HypeMetrics::Instance().ResetForTest(); }
    void TearDown() override { HypeMetrics::Instance().ResetForTest(); }
    HypeMetrics& m() { return HypeMetrics::Instance(); }
};

TEST_F(HypeFx, ToStringAll) {
    EXPECT_STREQ(ToString(LS::kSuccess), "success");
    EXPECT_STREQ(ToString(LS::kFailed), "failed");
    EXPECT_STREQ(ToString(HT::kChunk), "chunk");
    EXPECT_STREQ(ToString(HT::kHype), "hype");
    EXPECT_STREQ(ToString(HT::kBoth), "both");
}

class HypeLlmMatrix : public HypeFx, public ::testing::WithParamInterface<LS> {};
TEST_P(HypeLlmMatrix, Increments) {
    m().RecordLlmCall(GetParam());
    EXPECT_EQ(m().LlmCallCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, HypeLlmMatrix,
                         ::testing::Values(LS::kSuccess, LS::kFailed));

class HypeMatchMatrix : public HypeFx, public ::testing::WithParamInterface<HT> {};
TEST_P(HypeMatchMatrix, Increments) {
    m().RecordMatch(GetParam());
    m().RecordMatch(GetParam());
    EXPECT_EQ(m().MatchCount(GetParam()), 2u);
    for (HT h : {HT::kChunk, HT::kHype, HT::kBoth})
        if (h != GetParam()) EXPECT_EQ(m().MatchCount(h), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, HypeMatchMatrix,
                         ::testing::Values(HT::kChunk, HT::kHype, HT::kBoth));

TEST_F(HypeFx, QuestionsGeneratedCounter) {
    m().AddQuestionsGenerated(3);
    m().AddQuestionsGenerated(2);
    EXPECT_EQ(m().QuestionsGeneratedCount(), 5u);
}

// llm_duration histogram bounds {0.1,0.25,0.5,1,2,5,10} (7 finite + +Inf).
TEST_F(HypeFx, LlmDurationHistogram) {
    m().ObserveLlmDuration("gpt", 0.1);    // exactly first bound
    m().ObserveLlmDuration("gpt", 0.05);   // under
    m().ObserveLlmDuration("gpt", 999.0);  // above largest -> +Inf
    EXPECT_EQ(m().LlmDurationCount("gpt"), 3u);
    EXPECT_NEAR(m().LlmDurationSum("gpt"), 0.1 + 0.05 + 999.0, 1e-6);
    EXPECT_EQ(m().LlmDurationCount("unseen"), 0u);
}

TEST_F(HypeFx, RenderHelpTypeNoHighCardinality) {
    m().RecordLlmCall(LS::kSuccess);
    m().ObserveLlmDuration("gpt", 0.5);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_hype_index_llm_calls_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_hype_index_llm_calls_total{status=\"success\"} 1"),
              std::string::npos);
    EXPECT_EQ(out.find("chunk_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(HypeFx, Reset) {
    m().RecordLlmCall(LS::kSuccess);
    m().RecordMatch(HT::kChunk);
    m().AddQuestionsGenerated(4);
    m().ObserveLlmDuration("gpt", 0.3);
    m().ResetForTest();
    EXPECT_EQ(m().LlmCallCount(LS::kSuccess), 0u);
    EXPECT_EQ(m().MatchCount(HT::kChunk), 0u);
    EXPECT_EQ(m().QuestionsGeneratedCount(), 0u);
    EXPECT_EQ(m().LlmDurationCount("gpt"), 0u);
}

}  // namespace hype_matrix

// ====================== Contextual Retrieval ===========================
namespace ctx_matrix {

using CSt = ContextualRetrievalMetrics::ChunkStatus;
using QP = ContextualRetrievalMetrics::QueryPath;

class CtxFx : public ::testing::Test {
protected:
    void SetUp() override { ContextualRetrievalMetrics::Instance().ResetForTest(); }
    void TearDown() override {
        ContextualRetrievalMetrics::Instance().ResetForTest();
    }
    ContextualRetrievalMetrics& m() {
        return ContextualRetrievalMetrics::Instance();
    }
};

TEST_F(CtxFx, ToStringAll) {
    EXPECT_STREQ(ToString(CSt::kGenerated), "generated");
    EXPECT_STREQ(ToString(CSt::kFailed), "failed");
    EXPECT_STREQ(ToString(CSt::kSkippedNoLlm), "skipped_no_llm");
    EXPECT_STREQ(ToString(QP::kDense), "dense");
    EXPECT_STREQ(ToString(QP::kContextualized), "contextualized");
    EXPECT_STREQ(ToString(QP::kFallbackDense), "fallback_dense");
}

class CtxChunkMatrix : public CtxFx, public ::testing::WithParamInterface<CSt> {};
TEST_P(CtxChunkMatrix, Increments) {
    m().RecordChunk(GetParam());
    EXPECT_EQ(m().ChunkCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, CtxChunkMatrix,
                         ::testing::Values(CSt::kGenerated, CSt::kFailed,
                                           CSt::kSkippedNoLlm));

class CtxPathMatrix : public CtxFx, public ::testing::WithParamInterface<QP> {};
TEST_P(CtxPathMatrix, Increments) {
    m().RecordQueryPath(GetParam());
    EXPECT_EQ(m().QueryPathCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, CtxPathMatrix,
                         ::testing::Values(QP::kDense, QP::kContextualized,
                                           QP::kFallbackDense));

TEST_F(CtxFx, LlmCostTokensPerModel) {
    m().AddLlmCostTokens("gpt", 100);
    m().AddLlmCostTokens("gpt", 50);
    m().AddLlmCostTokens("claude", 30);
    EXPECT_EQ(m().LlmCostTokens("gpt"), 150);
    EXPECT_EQ(m().LlmCostTokens("claude"), 30);
    EXPECT_EQ(m().LlmCostTokens("unseen"), 0);
}

TEST_F(CtxFx, FallbackRatioGauge) {
    m().SetFallbackRatio(0.42);
    EXPECT_NEAR(m().FallbackRatio(), 0.42, 1e-5);
    m().SetFallbackRatio(0.1);
    EXPECT_NEAR(m().FallbackRatio(), 0.1, 1e-5);
}

TEST_F(CtxFx, RenderHelpTypeNoHighCardinality) {
    m().RecordChunk(CSt::kGenerated);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_contextual_retrieval_chunks_total counter"),
              std::string::npos);
    EXPECT_EQ(out.find("chunk_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(CtxFx, Reset) {
    m().RecordChunk(CSt::kGenerated);
    m().RecordQueryPath(QP::kDense);
    m().AddLlmCostTokens("gpt", 10);
    m().SetFallbackRatio(0.5);
    m().ResetForTest();
    EXPECT_EQ(m().ChunkCount(CSt::kGenerated), 0u);
    EXPECT_EQ(m().QueryPathCount(QP::kDense), 0u);
    EXPECT_EQ(m().LlmCostTokens("gpt"), 0);
    EXPECT_NEAR(m().FallbackRatio(), 0.0, 1e-9);
}

}  // namespace ctx_matrix

// ============================ Enricher =================================
namespace enricher_matrix {

using FR = EnricherMetrics::FallbackReason;

constexpr std::array<EnricherErrorCode, 6> kErrs{
    EnricherErrorCode::kInitFailed, EnricherErrorCode::kLlmTimeout,
    EnricherErrorCode::kLlmApi,     EnricherErrorCode::kParse,
    EnricherErrorCode::kBudget,     EnricherErrorCode::kRateLimit};
constexpr std::array<FR, 4> kFallbacks{FR::kApiKeyMissing, FR::kEndpointUnreachable,
                                       FR::kBudgetExceeded, FR::kCircuitOpen};

class EnricherFx : public ::testing::Test {
protected:
    void SetUp() override { EnricherMetrics::Instance().ResetForTest(); }
    void TearDown() override { EnricherMetrics::Instance().ResetForTest(); }
    EnricherMetrics& m() { return EnricherMetrics::Instance(); }
};

TEST_F(EnricherFx, ToStringFallbackReason) {
    EXPECT_STREQ(ToString(FR::kApiKeyMissing), "api_key_missing");
    EXPECT_STREQ(ToString(FR::kEndpointUnreachable), "endpoint_unreachable");
    EXPECT_STREQ(ToString(FR::kBudgetExceeded), "budget_exceeded");
    EXPECT_STREQ(ToString(FR::kCircuitOpen), "circuit_open");
}

class EnricherFailedTaskMatrix : public EnricherFx,
                         public ::testing::WithParamInterface<EnricherErrorCode> {
};
TEST_P(EnricherFailedTaskMatrix, Increments) {
    m().RecordFailedTask(GetParam());
    EXPECT_EQ(m().FailedTasksCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, EnricherFailedTaskMatrix, ::testing::ValuesIn(kErrs));

class EnricherFallbackMatrix : public EnricherFx, public ::testing::WithParamInterface<FR> {};
TEST_P(EnricherFallbackMatrix, Increments) {
    m().RecordFallbackToNull(GetParam());
    EXPECT_EQ(m().FallbackToNullCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, EnricherFallbackMatrix, ::testing::ValuesIn(kFallbacks));

TEST_F(EnricherFx, GaugesAndSimpleCounters) {
    m().SetCircuitBreakerState(2);
    EXPECT_EQ(m().CircuitBreakerState(), 2);
    m().RecordCircuitBreakerTrip();
    EXPECT_EQ(m().CircuitBreakerTripsCount(), 1u);
    m().RecordEndpointProbeFailed();
    EXPECT_EQ(m().EndpointProbeFailedCount(), 1u);
    m().RecordTruncated();
    m().RecordTruncated();
    EXPECT_EQ(m().TruncatedCount(), 2u);
    m().SetQueueDepth(11);
    EXPECT_EQ(m().QueueDepth(), 11);
}

TEST_F(EnricherFx, PerModelTokensAndCost) {
    m().RecordTokens("gpt", 100);
    m().RecordTokens("gpt", 50);
    m().RecordCostMicroUsd("gpt", 250000);  // 0.25 USD
    EXPECT_EQ(m().TokensCount("gpt"), 150);
    EXPECT_EQ(m().CostMicroUsd("gpt"), 250000);
    EXPECT_EQ(m().TokensCount("unseen"), 0);
}

// batch_size_actual histogram bounds {1,2,4,8,16,32}.
TEST_F(EnricherFx, BatchSizeHistogram) {
    m().ObserveBatchSizeActual(1);    // first bound
    m().ObserveBatchSizeActual(8);    // mid bound
    m().ObserveBatchSizeActual(999);  // above largest -> +Inf
    EXPECT_EQ(m().BatchSizeActualCount(), 3u);
}

// score_duration histogram bounds {0.1,...,30}s.
TEST_F(EnricherFx, ScoreDurationHistogram) {
    m().ObserveScoreDuration(0.1);    // exactly first bound
    m().ObserveScoreDuration(0.05);   // under
    m().ObserveScoreDuration(9999.0);  // above largest -> +Inf
    EXPECT_EQ(m().ScoreDurationCount(), 3u);
}

TEST_F(EnricherFx, ScoreDurationNegativeClamps) {
    m().ObserveScoreDuration(-1.0);
    EXPECT_EQ(m().ScoreDurationCount(), 1u);
}

TEST_F(EnricherFx, RenderHelpType) {
    m().RecordFailedTask(EnricherErrorCode::kLlmTimeout);
    m().RecordTokens("gpt", 5);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_enricher_failed_tasks_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_enricher_tokens_total counter"),
              std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(EnricherFx, Reset) {
    m().RecordFailedTask(EnricherErrorCode::kLlmTimeout);
    m().RecordFallbackToNull(FR::kCircuitOpen);
    m().SetCircuitBreakerState(2);
    m().RecordCircuitBreakerTrip();
    m().RecordTokens("gpt", 5);
    m().ObserveBatchSizeActual(8);
    m().ObserveScoreDuration(0.1);
    m().ResetForTest();
    EXPECT_EQ(m().FailedTasksCount(EnricherErrorCode::kLlmTimeout), 0u);
    EXPECT_EQ(m().FallbackToNullCount(FR::kCircuitOpen), 0u);
    EXPECT_EQ(m().CircuitBreakerState(), 0);
    EXPECT_EQ(m().CircuitBreakerTripsCount(), 0u);
    EXPECT_EQ(m().TokensCount("gpt"), 0);
    EXPECT_EQ(m().BatchSizeActualCount(), 0u);
    EXPECT_EQ(m().ScoreDurationCount(), 0u);
}

}  // namespace enricher_matrix

}  // namespace cortrix::spc

// ============================ ONNX =====================================
namespace cortrix::onnx {
namespace onnx_matrix {

using SR = OnnxMetrics::StartupResult;
constexpr std::array<SR, 4> kResults{SR::kOk, SR::kVersionMismatch,
                                     SR::kOpsetIncompatible,
                                     SR::kInferenceCheckFailed};

class OnnxFx : public ::testing::Test {
protected:
    void SetUp() override { OnnxMetrics::Instance().ResetForTest(); }
    void TearDown() override { OnnxMetrics::Instance().ResetForTest(); }
    OnnxMetrics& m() { return OnnxMetrics::Instance(); }
};

TEST_F(OnnxFx, ToStringAll) {
    EXPECT_STREQ(ToString(SR::kOk), "ok");
    EXPECT_STREQ(ToString(SR::kVersionMismatch), "version_mismatch");
    EXPECT_STREQ(ToString(SR::kOpsetIncompatible), "opset_incompatible");
    EXPECT_STREQ(ToString(SR::kInferenceCheckFailed), "inference_check_failed");
}

class OnnxStartupMatrix : public OnnxFx, public ::testing::WithParamInterface<SR> {};
TEST_P(OnnxStartupMatrix, Increments) {
    m().RecordStartupValidation(GetParam());
    EXPECT_EQ(m().StartupValidationCount(GetParam()), 1u);
    for (SR r : kResults)
        if (r != GetParam()) EXPECT_EQ(m().StartupValidationCount(r), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, OnnxStartupMatrix, ::testing::ValuesIn(kResults));

TEST_F(OnnxFx, InferenceFailedCounter) {
    m().RecordInferenceFailed();
    m().RecordInferenceFailed();
    EXPECT_EQ(m().InferenceFailedCount(), 2u);
}

// inference_duration histogram bounds {0.005,...,1}s.
TEST_F(OnnxFx, InferenceDurationHistogram) {
    m().ObserveInferenceDuration(0.005);  // exactly first bound
    m().ObserveInferenceDuration(0.002);  // under
    m().ObserveInferenceDuration(99.0);   // above largest -> +Inf
    EXPECT_EQ(m().InferenceDurationCount(), 3u);
    EXPECT_NEAR(m().InferenceDurationSum(), 0.005 + 0.002 + 99.0, 1e-6);
}

TEST_F(OnnxFx, RenderHelpTypeAndVersionInfo) {
    m().RecordStartupValidation(SR::kOk);
    m().RecordInferenceFailed();
    m().SetRuntimeVersionInfo(1, 18, 0);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_onnx_startup_validation_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_onnx_startup_validation_total{result=\"ok\"} 1"),
              std::string::npos);
    // single fixed label value retry_attempt="1".
    EXPECT_NE(out.find("cortrix_onnx_inference_failed_total{retry_attempt=\"1\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_onnx_runtime_version_info"), std::string::npos);
}

TEST_F(OnnxFx, Reset) {
    m().RecordStartupValidation(SR::kOk);
    m().RecordInferenceFailed();
    m().ObserveInferenceDuration(0.005);
    m().ResetForTest();
    EXPECT_EQ(m().StartupValidationCount(SR::kOk), 0u);
    EXPECT_EQ(m().InferenceFailedCount(), 0u);
    EXPECT_EQ(m().InferenceDurationCount(), 0u);
}

}  // namespace onnx_matrix
}  // namespace cortrix::onnx

// ============================ Scoring ==================================
namespace cortrix::scoring {
namespace scoring_matrix {

class ScoringFx : public ::testing::Test {
protected:
    void SetUp() override { ScoringMetrics::Instance().ResetForTest(); }
    void TearDown() override { ScoringMetrics::Instance().ResetForTest(); }
    ScoringMetrics& m() { return ScoringMetrics::Instance(); }
};

// score_total{level} — levels 0..4.
class ScoringLevelMatrix : public ScoringFx, public ::testing::WithParamInterface<uint8_t> {};
TEST_P(ScoringLevelMatrix, Increments) {
    m().RecordScore(GetParam());
    m().RecordScore(GetParam());
    EXPECT_EQ(m().ScoreCount(GetParam()), 2u);
}
INSTANTIATE_TEST_SUITE_P(All, ScoringLevelMatrix,
                         ::testing::Values<uint8_t>(0, 1, 2, 3, 4));

// error_total{code} — both F07 error codes.
class ScoringErrorMatrix : public ScoringFx,
                    public ::testing::WithParamInterface<F07ErrorCode> {};
TEST_P(ScoringErrorMatrix, Increments) {
    m().RecordError(GetParam());
    EXPECT_EQ(m().ErrorCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, ScoringErrorMatrix,
                         ::testing::Values(F07ErrorCode::kLevelInvalid,
                                           F07ErrorCode::kConfigInvalid));

TEST_F(ScoringFx, AnomalousAndFinalScoreCounters) {
    m().RecordAnomalous();
    m().RecordAnomalous();
    m().RecordFinalScore();
    EXPECT_EQ(m().AnomalousCount(), 2u);
    EXPECT_EQ(m().FinalScoreCount(), 1u);
}

TEST_F(ScoringFx, AssignDurationHistogramRender) {
    m().ObserveAssignDuration(0.01);
    m().ObserveAssignDuration(99.0);  // above largest -> +Inf
    std::string out = m().Render();
    EXPECT_NE(out.find("# TYPE cortrix_f07_assign_duration_seconds histogram"),
              std::string::npos);
}

TEST_F(ScoringFx, RenderEmitsAllFiveLevelsAndHelpType) {
    m().RecordScore(2);
    std::string out = m().Render();
    EXPECT_NE(out.find("# TYPE cortrix_f07_score_total counter"),
              std::string::npos);
    // all 5 level series emitted even when zero.
    for (int lv = 0; lv <= 4; ++lv) {
        EXPECT_NE(out.find("cortrix_f07_score_total{level=\"" +
                           std::to_string(lv) + "\"}"),
                  std::string::npos);
    }
    EXPECT_NE(out.find("# TYPE cortrix_f07_error_total counter"),
              std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(ScoringFx, Reset) {
    m().RecordScore(2);
    m().RecordAnomalous();
    m().RecordFinalScore();
    m().RecordError(F07ErrorCode::kLevelInvalid);
    m().ResetForTest();
    EXPECT_EQ(m().ScoreCount(2), 0u);
    EXPECT_EQ(m().AnomalousCount(), 0u);
    EXPECT_EQ(m().FinalScoreCount(), 0u);
    EXPECT_EQ(m().ErrorCount(F07ErrorCode::kLevelInvalid), 0u);
}

}  // namespace scoring_matrix
}  // namespace cortrix::scoring
