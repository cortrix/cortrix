#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/retrieval/crag_config.h"
#include "cortrix/retrieval/crag_error.h"
#include "cortrix/retrieval/crag_evaluator.h"
#include "cortrix/retrieval/heuristic_guard_backend.h"
#include "cortrix/retrieval/types.h"

// F37 S2/S3 coverage: CragEvaluator multi-signal feature engineering (§6.1),
// three-tier threshold classification (§6.2), heuristic guard (§7.3), and the L3
// retry-then-transparent-degrade path (§7.3). Standalone — backend is a stub /
// throwing mock, no ONNX.
namespace cortrix::retrieval {
namespace {

RankedChunk Chunk(const std::string& id, float score, const std::string& text = "t") {
    // RankedChunk{child_id, chunk_text, parent_text, score, rerank_score,
    //             score_signals, metadata}
    return RankedChunk{id, text, "p", score, 0.0f, {}, {}};
}

// A backend that always throws → exercises the L3 retry/degrade path.
class ThrowingBackend : public ICragClassifierBackend {
public:
    int calls = 0;
    CragBackendResult Infer(const std::string&, const std::string&,
                            const std::map<std::string, float>&) override {
        ++calls;
        throw CragInferenceError("forced failure");
    }
    std::string Name() const override { return "throwing"; }
    bool IsAvailable() const override { return true; }
};

// A backend returning a fixed verdict + confidence (for path/confidence tests).
class FixedBackend : public ICragClassifierBackend {
public:
    CragBackendResult result;
    int calls = 0;
    explicit FixedBackend(CragBackendResult r) : result(std::move(r)) {}
    CragBackendResult Infer(const std::string&, const std::string&,
                            const std::map<std::string, float>&) override {
        ++calls;
        return result;
    }
    std::string Name() const override { return "fixed"; }
    bool IsAvailable() const override { return true; }
};

constexpr float kEps = 1e-4f;

// ---------------- §6.1 multi-signal feature engineering ----------------

TEST(CragEvaluatorTest, MultiSignalsEmptyChunks) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    auto s = ev.ComputeMultiSignals({});
    EXPECT_FLOAT_EQ(s["top1"], 0.0f);
    EXPECT_FLOAT_EQ(s["median"], 0.0f);
    EXPECT_FLOAT_EQ(s["std"], 0.0f);
    EXPECT_FLOAT_EQ(s["high_score_ratio"], 0.0f);
    EXPECT_FLOAT_EQ(s["chunk_count"], 0.0f);
    // No top1_top5_gap for empty input.
    EXPECT_EQ(s.find("top1_top5_gap"), s.end());
}

TEST(CragEvaluatorTest, MultiSignalsComputesDistribution) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    std::vector<RankedChunk> chunks = {
        Chunk("a", 0.9f), Chunk("b", 0.7f), Chunk("c", 0.5f),
        Chunk("d", 0.3f), Chunk("e", 0.1f),
    };
    auto s = ev.ComputeMultiSignals(chunks);
    EXPECT_NEAR(s["top1"], 0.9f, kEps);
    EXPECT_NEAR(s["median"], 0.5f, kEps);          // middle of 5 sorted
    EXPECT_NEAR(s["chunk_count"], 5.0f, kEps);
    // scores >= 0.5: {0.9,0.7,0.5} = 3/5
    EXPECT_NEAR(s["high_score_ratio"], 0.6f, kEps);
    // top1-top5 gap present when >=5 candidates: 0.9 - 0.1 = 0.8
    ASSERT_NE(s.find("top1_top5_gap"), s.end());
    EXPECT_NEAR(s["top1_top5_gap"], 0.8f, kEps);
    EXPECT_GT(s["std"], 0.0f);
}

TEST(CragEvaluatorTest, MedianEvenCount) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    auto s = ev.ComputeMultiSignals({Chunk("a", 0.8f), Chunk("b", 0.4f)});
    EXPECT_NEAR(s["median"], 0.6f, kEps);  // (0.8+0.4)/2
    // <5 candidates → no gap signal.
    EXPECT_EQ(s.find("top1_top5_gap"), s.end());
}

TEST(CragEvaluatorTest, StdSingleElementIsZero) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    auto s = ev.ComputeMultiSignals({Chunk("a", 0.8f)});
    EXPECT_FLOAT_EQ(s["std"], 0.0f);
    EXPECT_NEAR(s["high_score_ratio"], 1.0f, kEps);
}

// ---------------- §6.2 three-tier classification (via heuristic backend) -------

TEST(CragEvaluatorTest, HighScoresClassifyCorrect) {
    // Strong top1 + full agreement → score ~0.97 → "correct" (>=0.7).
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    ClassifierInput in{"a real query", {Chunk("a", 0.95f), Chunk("b", 0.9f)}, std::nullopt};
    auto r = ev.Classify(in);
    EXPECT_EQ(r.label, "correct");
    EXPECT_GE(r.score, 0.7f);
    EXPECT_FALSE(r.error_code.has_value());
    // Signals carried through for ?explain=true.
    EXPECT_NE(r.raw_signals.find("top1"), r.raw_signals.end());
}

TEST(CragEvaluatorTest, MidScoresClassifyAmbiguous) {
    // top1=0.5, ratio=0.5 → 0.7*0.5+0.3*0.5 = 0.5 → in [0.3,0.7) → "ambiguous".
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    ClassifierInput in{"a real query", {Chunk("a", 0.5f), Chunk("b", 0.2f)}, std::nullopt};
    auto r = ev.Classify(in);
    EXPECT_EQ(r.label, "ambiguous");
    EXPECT_GE(r.score, 0.3f);
    EXPECT_LT(r.score, 0.7f);
}

TEST(CragEvaluatorTest, LowScoresClassifyIncorrect) {
    // top1=0.1, ratio=0 → 0.07 → < 0.3 → "incorrect".
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    ClassifierInput in{"a real query", {Chunk("a", 0.1f), Chunk("b", 0.05f)}, std::nullopt};
    auto r = ev.Classify(in);
    EXPECT_EQ(r.label, "incorrect");
    EXPECT_LT(r.score, 0.3f);
}

TEST(CragEvaluatorTest, NsThresholdOverrideShiftsVerdict) {
    // Same input, stricter threshold_correct → flips correct→ambiguous.
    std::vector<RankedChunk> chunks = {Chunk("a", 0.8f), Chunk("b", 0.8f)};
    ClassifierInput in{"a real query", chunks, std::nullopt};

    CragConfig strict;
    strict.threshold_correct = 0.95f;  // raise the bar
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), strict);
    auto r = ev.Classify(in);
    // 0.7*0.8+0.3*1.0 = 0.86 < 0.95 → not correct; >=0.3 → ambiguous.
    EXPECT_EQ(r.label, "ambiguous");
}

// ---------------- heuristic guard path (degenerate input / no backend) --------

TEST(CragEvaluatorTest, EmptyChunksTakesHeuristicGuard) {
    auto backend = std::make_shared<FixedBackend>(
        CragBackendResult{"correct", 0.99f, 1.0f});
    CragEvaluator ev(backend, CragConfig{});
    ClassifierInput in{"a real query", {}, std::nullopt};
    auto r = ev.Classify(in);
    // Guard path: backend NOT called for empty chunks.
    EXPECT_EQ(backend->calls, 0);
    EXPECT_EQ(r.label, "incorrect");  // all-zero signals → score 0
}

TEST(CragEvaluatorTest, TinyQueryTakesHeuristicGuard) {
    auto backend = std::make_shared<FixedBackend>(
        CragBackendResult{"correct", 0.99f, 1.0f});
    CragEvaluator ev(backend, CragConfig{});
    ClassifierInput in{"hi", {Chunk("a", 0.9f)}, std::nullopt};  // query len < 3
    auto r = ev.Classify(in);
    EXPECT_EQ(backend->calls, 0);  // guard, not backend
    EXPECT_EQ(r.label, "correct");
}

TEST(CragEvaluatorTest, NullBackendNotAvailableUsesGuard) {
    CragEvaluator ev(nullptr, CragConfig{});
    EXPECT_FALSE(ev.IsAvailable());
    ClassifierInput in{"a real query", {Chunk("a", 0.9f)}, std::nullopt};
    auto r = ev.Classify(in);
    EXPECT_EQ(r.label, "correct");  // guard ran
    EXPECT_FALSE(r.error_code.has_value());
}

TEST(CragEvaluatorTest, LowConfidenceBackendFallsBackToGuard) {
    // Backend reports a verdict but with confidence below the min → guard takes over.
    CragConfig cfg;
    cfg.classifier_min_confidence = 0.8f;
    auto backend = std::make_shared<FixedBackend>(
        CragBackendResult{"incorrect", 0.1f, /*confidence=*/0.2f});
    CragEvaluator ev(backend, cfg);
    ClassifierInput in{"a real query", {Chunk("a", 0.95f), Chunk("b", 0.9f)}, std::nullopt};
    auto r = ev.Classify(in);
    EXPECT_EQ(backend->calls, 1);   // backend WAS called
    EXPECT_EQ(r.label, "correct");  // but guard verdict (high signals) won
}

// ---------------- §7.3 L3 retry-then-degrade ----------------------------------

TEST(CragEvaluatorTest, InferenceFailureDegradesTransparently) {
    CragConfig cfg;
    cfg.max_inference_retries = 3;
    auto backend = std::make_shared<ThrowingBackend>();
    CragEvaluator ev(backend, cfg);
    ClassifierInput in{"a real query", {Chunk("a", 0.9f), Chunk("b", 0.8f)}, std::nullopt};
    auto r = ev.Classify(in);
    // 1 initial + 3 retries = 4 attempts.
    EXPECT_EQ(backend->calls, 4);
    EXPECT_EQ(r.label, "correct_fallback_classifier_failed");
    ASSERT_TRUE(r.error_code.has_value());
    EXPECT_EQ(*r.error_code, "CX_ERR_F37_INFERENCE_FAILED");
    EXPECT_FLOAT_EQ(r.score, 0.5f);
    // Signals preserved for ?explain=true.
    EXPECT_NE(r.raw_signals.find("top1"), r.raw_signals.end());
}

TEST(CragEvaluatorTest, ZeroRetriesDegradesAfterSingleAttempt) {
    CragConfig cfg;
    cfg.max_inference_retries = 0;
    auto backend = std::make_shared<ThrowingBackend>();
    CragEvaluator ev(backend, cfg);
    ClassifierInput in{"a real query", {Chunk("a", 0.9f)}, std::nullopt};
    auto r = ev.Classify(in);
    EXPECT_EQ(backend->calls, 1);
    EXPECT_EQ(r.label, "correct_fallback_classifier_failed");
}

TEST(CragEvaluatorTest, BackendExplicitLabelIsHonored) {
    // Backend returns a non-empty label → CragEvaluator keeps it (does not re-derive
    // from score). Covers the `br.label.empty() ? ... : br.label` false branch.
    auto backend = std::make_shared<FixedBackend>(
        CragBackendResult{"ambiguous", /*score=*/0.95f, /*confidence=*/1.0f});
    CragEvaluator ev(backend, CragConfig{});
    ClassifierInput in{"a real query", {Chunk("a", 0.95f)}, std::nullopt};
    auto r = ev.Classify(in);
    EXPECT_EQ(backend->calls, 1);
    EXPECT_EQ(r.label, "ambiguous");        // explicit backend label wins over 0.95→correct
    EXPECT_NEAR(r.score, 0.95f, kEps);
}

TEST(CragEvaluatorTest, BackendScoreClampedToUnitRange) {
    // Backend returns an out-of-range score → clamped to [0,1]. Covers the clamp.
    auto over = std::make_shared<FixedBackend>(
        CragBackendResult{"", /*score=*/1.7f, /*confidence=*/1.0f});
    CragEvaluator ev_over(over, CragConfig{});
    auto r_over = ev_over.Classify({"a real query", {Chunk("a", 0.9f)}, std::nullopt});
    EXPECT_NEAR(r_over.score, 1.0f, kEps);
    EXPECT_EQ(r_over.label, "correct");

    auto under = std::make_shared<FixedBackend>(
        CragBackendResult{"", /*score=*/-0.3f, /*confidence=*/1.0f});
    CragEvaluator ev_under(under, CragConfig{});
    auto r_under = ev_under.Classify({"a real query", {Chunk("a", 0.9f)}, std::nullopt});
    EXPECT_NEAR(r_under.score, 0.0f, kEps);
    EXPECT_EQ(r_under.label, "incorrect");
}

TEST(CragEvaluatorTest, BoundaryScoresAtThresholds) {
    // Exact-threshold behaviour (§6.2): >= threshold_correct → correct; the lower
    // boundary uses strict < threshold_incorrect for incorrect.
    CragConfig cfg;  // 0.7 / 0.3 defaults
    // score exactly 0.7 → correct (>=). Build signals so heuristic score == 0.7:
    // 0.7*top1 + 0.3*ratio = 0.7 with top1=1.0, ratio=0 → ratio 0 needs scores<0.5.
    auto backend = std::make_shared<FixedBackend>(
        CragBackendResult{"", /*score=*/0.7f, /*confidence=*/1.0f});
    CragEvaluator ev(backend, cfg);
    auto correct = ev.Classify({"a real query", {Chunk("a", 0.9f)}, std::nullopt});
    EXPECT_EQ(correct.label, "correct");

    auto at_incorrect = std::make_shared<FixedBackend>(
        CragBackendResult{"", /*score=*/0.3f, /*confidence=*/1.0f});
    CragEvaluator ev2(at_incorrect, cfg);
    auto amb = ev2.Classify({"a real query", {Chunk("a", 0.9f)}, std::nullopt});
    EXPECT_EQ(amb.label, "ambiguous");  // 0.3 is NOT < 0.3 → ambiguous boundary
}

TEST(CragEvaluatorTest, CircuitBreakerOpensThenGatesSubsequentQuery) {
    // Breaker threshold = 3 (ctor). Query 1 fails 1+3=4 times → trips the breaker
    // open. Query 2 then sees the breaker open at entry → backend NOT called,
    // straight to the transparent degrade (systemic gate, §7.3 / F02 breaker).
    CragConfig cfg;
    cfg.max_inference_retries = 3;
    auto backend = std::make_shared<ThrowingBackend>();
    CragEvaluator ev(backend, cfg);
    ClassifierInput in{"a real query", {Chunk("a", 0.9f), Chunk("b", 0.8f)}, std::nullopt};

    auto r1 = ev.Classify(in);
    EXPECT_EQ(r1.label, "correct_fallback_classifier_failed");
    const int after_q1 = backend->calls;
    EXPECT_GE(after_q1, 3);  // enough failures to trip the breaker

    auto r2 = ev.Classify(in);
    EXPECT_EQ(r2.label, "correct_fallback_classifier_failed");
    ASSERT_TRUE(r2.error_code.has_value());
    EXPECT_EQ(*r2.error_code, "CX_ERR_F37_INFERENCE_FAILED");
    // Breaker open → query 2 made no additional backend calls.
    EXPECT_EQ(backend->calls, after_q1);
}

// ---------------- IClassifier contract ----------------------------------------

TEST(CragEvaluatorTest, NameAndLabels) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    EXPECT_EQ(ev.Name(), "f37-crag");
    EXPECT_EQ(ev.Labels(),
              (std::vector<std::string>{"correct", "ambiguous", "incorrect"}));
    EXPECT_TRUE(ev.IsAvailable());
}

TEST(CragConfigTest, IsValidEnforcesThresholdInvariant) {
    EXPECT_TRUE(CragConfig{}.IsValid());  // 0.3 <= 0.7, both in range
    CragConfig bad_order;
    bad_order.threshold_correct = 0.2f;
    bad_order.threshold_incorrect = 0.6f;  // incorrect > correct → invalid
    EXPECT_FALSE(bad_order.IsValid());
    CragConfig out_of_range;
    out_of_range.threshold_correct = 1.5f;
    EXPECT_FALSE(out_of_range.IsValid());
    CragConfig negative;
    negative.threshold_incorrect = -0.1f;
    EXPECT_FALSE(negative.IsValid());
}

}  // namespace
}  // namespace cortrix::retrieval
