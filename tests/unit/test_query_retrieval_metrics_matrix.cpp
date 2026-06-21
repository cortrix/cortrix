// Query + retrieval metrics — exhaustive matrix coverage for QueryRouter / Crag /
// Sparse / Scatter / RagFusion (enum ToString, label-combo counters, gauges,
// histogram bucket boundaries via the public bucket accessors, render, reset).
// Separate suite namespaces from existing per-feature metric tests.
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <tuple>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/query/query_router_metrics.h"
#include "cortrix/query/rag_fusion_metrics.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/query/scatter_metrics.h"
#include "cortrix/retrieval/crag_metrics.h"
#include "cortrix/retrieval/sparse_metrics.h"

// ============================ QueryRouter ====================================
namespace cortrix::query {
namespace qr_matrix {

using D = QueryRouterMetrics::Decision;
using P = QueryRouterMetrics::SavedPath;

constexpr std::array<D, 4> kDecisions{D::kSimple, D::kComplex, D::kChat,
                                      D::kFallback};

class RouterFx : public ::testing::Test {
protected:
    void SetUp() override { QueryRouterMetrics::Instance().ResetForTest(); }
    void TearDown() override { QueryRouterMetrics::Instance().ResetForTest(); }
    QueryRouterMetrics& m() { return QueryRouterMetrics::Instance(); }
};

TEST_F(RouterFx, ToStringAll) {
    EXPECT_STREQ(ToString(D::kSimple), "simple");
    EXPECT_STREQ(ToString(D::kComplex), "complex");
    EXPECT_STREQ(ToString(D::kChat), "chat");
    EXPECT_STREQ(ToString(D::kFallback), "fallback");
    EXPECT_STREQ(ToString(P::kSimple), "simple");
    EXPECT_STREQ(ToString(P::kChat), "chat");
}

class RouterDecisionMatrix : public RouterFx, public ::testing::WithParamInterface<D> {};
TEST_P(RouterDecisionMatrix, IncrementsOnlyTargetCell) {
    m().RecordDecision(GetParam());
    m().RecordDecision(GetParam());
    EXPECT_EQ(m().DecisionCount(GetParam()), 2u);
    for (D d : kDecisions)
        if (d != GetParam()) EXPECT_EQ(m().DecisionCount(d), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, RouterDecisionMatrix, ::testing::ValuesIn(kDecisions));

TEST_F(RouterFx, ComputeSavedPerPath) {
    m().AddComputeSaved(P::kSimple, 1.5);
    m().AddComputeSaved(P::kSimple, 0.5);
    m().AddComputeSaved(P::kChat, 2.0);
    EXPECT_NEAR(m().ComputeSavedSeconds(P::kSimple), 2.0, 1e-3);
    EXPECT_NEAR(m().ComputeSavedSeconds(P::kChat), 2.0, 1e-3);
}

TEST_F(RouterFx, FallbackRatioGaugeLastWriteWins) {
    m().SetFallbackRatio(0.04);
    EXPECT_NEAR(m().FallbackRatio(), 0.04, 1e-4);
    m().SetFallbackRatio(0.10);
    EXPECT_NEAR(m().FallbackRatio(), 0.10, 1e-4);
}

TEST_F(RouterFx, ClassifierLatencyHistogram) {
    m().ObserveClassifierLatency(0.0001);
    m().ObserveClassifierLatency(0.2);
    m().ObserveClassifierLatency(9999.0);  // above largest -> +Inf only
    EXPECT_EQ(m().ClassifierLatencyCount(), 3u);
    EXPECT_NEAR(m().ClassifierLatencySum(), 0.0001 + 0.2 + 9999.0, 1.0);
}

TEST_F(RouterFx, ClassifierLatencyNegativeClamps) {
    m().ObserveClassifierLatency(-1.0);
    EXPECT_EQ(m().ClassifierLatencyCount(), 1u);
}

TEST_F(RouterFx, RenderHelpTypeNoHighCardinality) {
    m().RecordDecision(D::kSimple);
    m().SetFallbackRatio(0.02);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_query_router_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_query_router_total{decision=\"simple\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_query_router_fallback_ratio gauge"),
              std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(RouterFx, Reset) {
    m().RecordDecision(D::kSimple);
    m().AddComputeSaved(P::kSimple, 1.0);
    m().SetFallbackRatio(0.5);
    m().ObserveClassifierLatency(0.1);
    m().ResetForTest();
    EXPECT_EQ(m().DecisionCount(D::kSimple), 0u);
    EXPECT_NEAR(m().ComputeSavedSeconds(P::kSimple), 0.0, 1e-6);
    EXPECT_NEAR(m().FallbackRatio(), 0.0, 1e-6);
    EXPECT_EQ(m().ClassifierLatencyCount(), 0u);
}

}  // namespace qr_matrix

// ============================ RagFusion ======================================
namespace rf_matrix {

using R = RagFusionMetrics::Result;
using DR = RagFusionMetrics::DegradeReason;
using TD = RagFusionMetrics::TokenDirection;

constexpr std::array<R, 3> kResults{R::kSuccess, R::kDegraded, R::kDisabled};
constexpr std::array<DR, 5> kReasons{DR::kLlmTimeout, DR::kCircuitOpen,
                                     DR::kQuotaExceeded, DR::kInvalidResponse,
                                     DR::kOther};

class RagFusionFx : public ::testing::Test {
protected:
    void SetUp() override { RagFusionMetrics::Instance().ResetForTest(); }
    void TearDown() override { RagFusionMetrics::Instance().ResetForTest(); }
    RagFusionMetrics& m() { return RagFusionMetrics::Instance(); }
};

TEST_F(RagFusionFx, ToStringAll) {
    EXPECT_STREQ(ToString(R::kSuccess), "success");
    EXPECT_STREQ(ToString(R::kDegraded), "degraded");
    EXPECT_STREQ(ToString(R::kDisabled), "disabled");
    EXPECT_STREQ(ToString(DR::kLlmTimeout), "llm_timeout");
    EXPECT_STREQ(ToString(DR::kCircuitOpen), "circuit_open");
    EXPECT_STREQ(ToString(DR::kQuotaExceeded), "quota_exceeded");
    EXPECT_STREQ(ToString(DR::kInvalidResponse), "invalid_response");
    EXPECT_STREQ(ToString(DR::kOther), "other");
    EXPECT_STREQ(ToString(TD::kInput), "input");
    EXPECT_STREQ(ToString(TD::kOutput), "output");
}

class RagFusionResultMatrix : public RagFusionFx, public ::testing::WithParamInterface<R> {};
TEST_P(RagFusionResultMatrix, Increments) {
    m().RecordInvocation(GetParam());
    EXPECT_EQ(m().InvocationCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, RagFusionResultMatrix, ::testing::ValuesIn(kResults));

class RagFusionReasonMatrix : public RagFusionFx, public ::testing::WithParamInterface<DR> {};
TEST_P(RagFusionReasonMatrix, Increments) {
    m().RecordDegraded(GetParam());
    EXPECT_EQ(m().DegradedCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, RagFusionReasonMatrix, ::testing::ValuesIn(kReasons));

TEST_F(RagFusionFx, TokensPerDirection) {
    m().RecordTokens(TD::kInput, 100);
    m().RecordTokens(TD::kOutput, 30);
    EXPECT_EQ(m().TokenCount(TD::kInput), 100u);
    EXPECT_EQ(m().TokenCount(TD::kOutput), 30u);
}

// variant_count histogram bounds {1,2,3,5,10}; bucket index inspect.
TEST_F(RagFusionFx, VariantCountBucketPlacement) {
    VariantStrategy s = VariantStrategy::kParaphrase;
    m().ObserveVariantCount(s, 3);   // exactly bound index 2 (le=3)
    m().ObserveVariantCount(s, 1);   // exactly first bound (le=1)
    m().ObserveVariantCount(s, 99);  // above largest -> +Inf bucket
    EXPECT_EQ(m().VariantCountObservations(s), 3u);
    EXPECT_EQ(m().VariantCountSum(s), 103u);
    // Per-bucket (non-cumulative) raw counts. le=1 (index 0) holds value 1.
    EXPECT_EQ(m().VariantCountBucket(s, 0), 1u);
    // le=3 (index 2) holds value 3.
    EXPECT_EQ(m().VariantCountBucket(s, 2), 1u);
    // +Inf (index kVariantBuckets=5) holds value 99.
    EXPECT_EQ(m().VariantCountBucket(s, 5), 1u);
}

// rrf_fusion_duration histogram bounds {1e-4,1e-3,1e-2,0.1,1}s (passed as us).
TEST_F(RagFusionFx, RrfFusionBucketPlacement) {
    m().ObserveRrfFusionDuration(100);      // 1e-4 s = exactly first bound
    m().ObserveRrfFusionDuration(50);       // under first bound
    m().ObserveRrfFusionDuration(99999999);  // far above largest -> +Inf only
    EXPECT_EQ(m().RrfFusionDurationCount(), 3u);
    EXPECT_EQ(m().RrfFusionDurationSumUs(), 100u + 50u + 99999999u);
    // Per-bucket (non-cumulative). le index 0 (1e-4 s) holds the two small samples.
    EXPECT_EQ(m().RrfFusionDurationBucket(0), 2u);
    EXPECT_EQ(m().RrfFusionDurationBucket(5), 1u);  // +Inf holds the huge sample
}

TEST_F(RagFusionFx, RrfFusionNegativeClamps) {
    m().ObserveRrfFusionDuration(-5);
    EXPECT_EQ(m().RrfFusionDurationCount(), 1u);
    EXPECT_EQ(m().RrfFusionDurationBucket(0), 1u);  // smallest bucket
}

TEST_F(RagFusionFx, RenderHelpTypeNoHighCardinality) {
    m().RecordInvocation(R::kSuccess);
    m().RecordTokens(TD::kInput, 1);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_rag_fusion_invocation_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_rag_fusion_invocation_total{result=\"success\"} 1"),
              std::string::npos);
    EXPECT_EQ(out.find("tenant_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
}

TEST_F(RagFusionFx, Reset) {
    m().RecordInvocation(R::kSuccess);
    m().RecordDegraded(DR::kOther);
    m().RecordTokens(TD::kInput, 5);
    m().ObserveVariantCount(VariantStrategy::kParaphrase, 3);
    m().ObserveRrfFusionDuration(100);
    m().ResetForTest();
    EXPECT_EQ(m().InvocationCount(R::kSuccess), 0u);
    EXPECT_EQ(m().DegradedCount(DR::kOther), 0u);
    EXPECT_EQ(m().TokenCount(TD::kInput), 0u);
    EXPECT_EQ(m().VariantCountObservations(VariantStrategy::kParaphrase), 0u);
    EXPECT_EQ(m().RrfFusionDurationCount(), 0u);
}

}  // namespace rf_matrix

// ============================ Scatter ========================================
namespace scatter_matrix {

using Reason = ScatterMetrics::Reason;
using Cat = agent_friendly::ErrorCategory;

constexpr std::array<Reason, 3> kReasons{Reason::kSingle, Reason::kMulti,
                                         Reason::kWildcard};
constexpr std::array<Cat, 5> kCats{Cat::kAuth, Cat::kQuota, Cat::kTransient,
                                   Cat::kPermanent, Cat::kTimeout};

class ScatterFx : public ::testing::Test {
protected:
    void SetUp() override { ScatterMetrics::Instance().ResetForTest(); }
    void TearDown() override { ScatterMetrics::Instance().ResetForTest(); }
    ScatterMetrics& m() { return ScatterMetrics::Instance(); }
};

TEST_F(ScatterFx, ToStringReason) {
    EXPECT_STREQ(ToString(Reason::kSingle), "single");
    EXPECT_STREQ(ToString(Reason::kMulti), "multi");
    EXPECT_STREQ(ToString(Reason::kWildcard), "wildcard");
}

using ReqParam = std::tuple<Reason, Cat>;
class ScatterRequestMatrix : public ScatterFx, public ::testing::WithParamInterface<ReqParam> {};
TEST_P(ScatterRequestMatrix, IncrementsOnlyTargetCell) {
    auto [r, c] = GetParam();
    m().RecordRequest(r, c);
    m().RecordFailed(r, c);
    EXPECT_EQ(m().RequestCount(r, c), 1u);
    EXPECT_EQ(m().FailedCount(r, c), 1u);
    for (Reason rr : kReasons)
        for (Cat cc : kCats)
            if (!(rr == r && cc == c)) {
                EXPECT_EQ(m().RequestCount(rr, cc), 0u);
                EXPECT_EQ(m().FailedCount(rr, cc), 0u);
            }
}
INSTANTIATE_TEST_SUITE_P(AllCells, ScatterRequestMatrix,
                         ::testing::Combine(::testing::ValuesIn(kReasons),
                                            ::testing::ValuesIn(kCats)));

TEST_F(ScatterFx, NsPerQueryBucketPlacement) {
    // bounds {1,3,10,100} (count-type).
    m().ObserveNamespacesPerQuery(1);    // exactly le=1
    m().ObserveNamespacesPerQuery(3);    // exactly le=3
    m().ObserveNamespacesPerQuery(500);  // above largest -> +Inf
    EXPECT_EQ(m().NamespacesPerQueryCount(), 3u);
    EXPECT_EQ(m().NamespacesPerQuerySum(), 504u);
    // Per-bucket (non-cumulative) raw counts.
    EXPECT_EQ(m().NamespacesPerQueryBucket(0), 1u);  // value 1 -> le=1
    EXPECT_EQ(m().NamespacesPerQueryBucket(1), 1u);  // value 3 -> le=3
    EXPECT_EQ(m().NamespacesPerQueryBucket(4), 1u);  // value 500 -> +Inf
}

TEST_F(ScatterFx, DurationBucketMapping) {
    EXPECT_EQ(ScatterMetrics::DurationBucket(1), 0);
    EXPECT_EQ(ScatterMetrics::DurationBucket(3), 1);
    EXPECT_EQ(ScatterMetrics::DurationBucket(10), 2);
    EXPECT_EQ(ScatterMetrics::DurationBucket(50), 3);
}

TEST_F(ScatterFx, DurationHistogramLatencyBuckets) {
    // ns_count=1 -> bucket 0; latency bounds {0.1,...,30}s.
    m().ObserveDuration(1, 100);      // 0.1s exactly first le
    m().ObserveDuration(1, 50);       // under
    m().ObserveDuration(1, 999000);   // above largest -> +Inf
    EXPECT_EQ(m().DurationCount(0), 3u);
    // Per-bucket (non-cumulative) raw counts.
    EXPECT_EQ(m().DurationLatencyBucket(0, 0), 2u);  // le=0.1 holds the two ≤0.1
    EXPECT_EQ(m().DurationLatencyBucket(0, 8), 1u);  // +Inf holds the huge sample
}

TEST_F(ScatterFx, DedupAndPartialCounters) {
    m().RecordDedupCollisions(5);
    m().RecordDedupCollisions(2);
    m().RecordPartialSuccess();
    EXPECT_EQ(m().DedupCollisionsCount(), 7u);
    EXPECT_EQ(m().PartialSuccessCount(), 1u);
}

TEST_F(ScatterFx, RenderHelpType) {
    m().RecordRequest(Reason::kSingle, Cat::kPermanent);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_scatter_requests_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_scatter_namespaces_per_query histogram"),
              std::string::npos);
}

TEST_F(ScatterFx, Reset) {
    m().RecordRequest(Reason::kSingle, Cat::kPermanent);
    m().RecordFailed(Reason::kMulti, Cat::kTimeout);
    m().ObserveNamespacesPerQuery(5);
    m().RecordDedupCollisions(3);
    m().RecordPartialSuccess();
    m().ObserveDuration(1, 100);
    m().ResetForTest();
    EXPECT_EQ(m().RequestCount(Reason::kSingle, Cat::kPermanent), 0u);
    EXPECT_EQ(m().FailedCount(Reason::kMulti, Cat::kTimeout), 0u);
    EXPECT_EQ(m().NamespacesPerQueryCount(), 0u);
    EXPECT_EQ(m().DedupCollisionsCount(), 0u);
    EXPECT_EQ(m().PartialSuccessCount(), 0u);
    EXPECT_EQ(m().DurationCount(0), 0u);
}

}  // namespace scatter_matrix
}  // namespace cortrix::query

// ============================ Crag ===========================================
namespace cortrix::retrieval {
namespace crag_matrix {

using D = CragMetrics::Decision;
constexpr std::array<D, 4> kDecisions{D::kCorrect, D::kAmbiguous, D::kIncorrect,
                                      D::kFallback};

class CragFx : public ::testing::Test {
protected:
    void SetUp() override { CragMetrics::Instance().ResetForTest(); }
    void TearDown() override { CragMetrics::Instance().ResetForTest(); }
    CragMetrics& m() { return CragMetrics::Instance(); }
};

TEST_F(CragFx, ToStringAll) {
    EXPECT_STREQ(ToString(D::kCorrect), "correct");
    EXPECT_STREQ(ToString(D::kAmbiguous), "ambiguous");
    EXPECT_STREQ(ToString(D::kIncorrect), "incorrect");
    EXPECT_STREQ(ToString(D::kFallback), "fallback");
}

class CragDecisionMatrix : public CragFx, public ::testing::WithParamInterface<D> {};
TEST_P(CragDecisionMatrix, Increments) {
    m().RecordEvaluation(GetParam());
    EXPECT_EQ(m().EvaluationCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, CragDecisionMatrix, ::testing::ValuesIn(kDecisions));

TEST_F(CragFx, GaugesLastWriteWins) {
    m().SetFallbackRatio(0.03);
    m().SetIncorrectRatio(0.07);
    EXPECT_NEAR(m().FallbackRatio(), 0.03, 1e-4);
    EXPECT_NEAR(m().IncorrectRatio(), 0.07, 1e-4);
    m().SetFallbackRatio(0.10);
    EXPECT_NEAR(m().FallbackRatio(), 0.10, 1e-4);
}

TEST_F(CragFx, ClassifierLatencyHistogram) {
    m().ObserveClassifierLatency(0.001);
    m().ObserveClassifierLatency(9999.0);  // above largest -> +Inf
    EXPECT_EQ(m().ClassifierLatencyCount(), 2u);
    EXPECT_NEAR(m().ClassifierLatencySum(), 0.001 + 9999.0, 1.0);
}

TEST_F(CragFx, ClassifierLatencyNegativeClamps) {
    m().ObserveClassifierLatency(-1.0);
    EXPECT_EQ(m().ClassifierLatencyCount(), 1u);
}

TEST_F(CragFx, RenderHelpTypeNoHighCardinality) {
    m().RecordEvaluation(D::kCorrect);
    m().SetFallbackRatio(0.01);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_crag_evaluation_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_crag_evaluation_total{decision=\"correct\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_crag_fallback_ratio gauge"),
              std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(CragFx, Reset) {
    m().RecordEvaluation(D::kCorrect);
    m().SetFallbackRatio(0.5);
    m().SetIncorrectRatio(0.5);
    m().ObserveClassifierLatency(0.01);
    m().ResetForTest();
    EXPECT_EQ(m().EvaluationCount(D::kCorrect), 0u);
    EXPECT_NEAR(m().FallbackRatio(), 0.0, 1e-6);
    EXPECT_NEAR(m().IncorrectRatio(), 0.0, 1e-6);
    EXPECT_EQ(m().ClassifierLatencyCount(), 0u);
}

}  // namespace crag_matrix

// ============================ Sparse =========================================
namespace sparse_matrix {

using IS = SparseMetrics::InferenceStatus;
using VP = SparseMetrics::ViaPath;

constexpr std::array<IS, 3> kStatuses{IS::kSuccess, IS::kFailed, IS::kFallback};
constexpr std::array<VP, 2> kPaths{VP::kSparse, VP::kFallbackDenseFts5};

class SparseFx : public ::testing::Test {
protected:
    void SetUp() override { SparseMetrics::Instance().ResetForTest(); }
    void TearDown() override { SparseMetrics::Instance().ResetForTest(); }
    SparseMetrics& m() { return SparseMetrics::Instance(); }
};

TEST_F(SparseFx, ToStringAll) {
    EXPECT_STREQ(ToString(IS::kSuccess), "success");
    EXPECT_STREQ(ToString(IS::kFailed), "failed");
    EXPECT_STREQ(ToString(IS::kFallback), "fallback");
    EXPECT_STREQ(ToString(VP::kSparse), "sparse");
    EXPECT_STREQ(ToString(VP::kFallbackDenseFts5), "fallback_dense_fts5");
}

class SparseInferMatrix : public SparseFx, public ::testing::WithParamInterface<IS> {};
TEST_P(SparseInferMatrix, Increments) {
    m().RecordInference(GetParam());
    EXPECT_EQ(m().InferenceCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, SparseInferMatrix, ::testing::ValuesIn(kStatuses));

class SparsePathMatrix : public SparseFx, public ::testing::WithParamInterface<VP> {};
TEST_P(SparsePathMatrix, Increments) {
    m().RecordQueryViaPath(GetParam());
    EXPECT_EQ(m().QueryViaPathCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, SparsePathMatrix, ::testing::ValuesIn(kPaths));

TEST_F(SparseFx, GaugesLastWriteWins) {
    m().SetFallbackRatio(0.25);
    EXPECT_NEAR(m().FallbackRatio(), 0.25, 1e-4);
    m().SetInvertedIndexSizeBytes(4096);
    EXPECT_EQ(m().InvertedIndexSizeBytes(), 4096u);
}

TEST_F(SparseFx, RenderHelpType) {
    m().RecordInference(IS::kSuccess);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_bge_m3_sparse_inference_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_bge_m3_sparse_inference_total{status=\"success\"} 1"),
              std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(SparseFx, Reset) {
    m().RecordInference(IS::kSuccess);
    m().RecordQueryViaPath(VP::kSparse);
    m().SetFallbackRatio(0.5);
    m().SetInvertedIndexSizeBytes(100);
    m().ResetForTest();
    EXPECT_EQ(m().InferenceCount(IS::kSuccess), 0u);
    EXPECT_EQ(m().QueryViaPathCount(VP::kSparse), 0u);
    EXPECT_NEAR(m().FallbackRatio(), 0.0, 1e-6);
    EXPECT_EQ(m().InvertedIndexSizeBytes(), 0u);
}

}  // namespace sparse_matrix
}  // namespace cortrix::retrieval
