#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

#include "cortrix/reranker/circuit_breaker.h"
#include "cortrix/retrieval/crag_config.h"
#include "cortrix/retrieval/crag_evaluator.h"
#include "cortrix/retrieval/heuristic_guard_backend.h"
#include "cortrix/retrieval/types.h"

// DEPTH — two anti-regression gaps the coverage mapping flagged:
//
// (1) CragBreakerDepth: the circuit-breaker COOLDOWN → half-open → re-close
//     recovery cycle (the breaker re-closes after cooldown). The F37
//     CragEvaluator owns a reranker::CircuitBreaker but exposes neither the
//     breaker nor a clock seam, and CragConfig has no cooldown field — so the
//     breaker recovery is NOT deterministically reachable THROUGH CragEvaluator
//     (cooldown would be wall-clock; see REPORT note). The breaker itself DOES
//     take an injectable NowFn, so this suite drives it directly with a fake
//     clock for the full open→half_open→closed recovery + re-trip, the
//     OnStateChange transition callback sequence, OpenSinceMs, and the exact
//     cooldown boundary (==cooldown vs <cooldown) that the existing
//     test_reranker_circuit_breaker.cpp does not pin.
//
// (2) CragSignalsDepth: the top1_top5_gap off-by-one boundary (size==4 → absent,
//     size==5 → present with the exact scores[0]-scores[4] value). The existing
//     test_crag_evaluator.cpp tests size 5 and size 2 but never the 4/5 edge.
//
// Suite names are globally unique (CragBreakerDepth / CragSignalsDepth).
namespace cortrix::retrieval {
namespace {

// ---------------- (1) CircuitBreaker recovery with a fake clock ----------------

using reranker::CircuitBreaker;
using reranker::CircuitBreakerState;

struct FakeClock {
    int64_t now_ms = 0;
    CircuitBreaker::NowFn fn() { return [this] { return now_ms; }; }
    void Advance(int64_t ms) { now_ms += ms; }
};

// Trip closed → open by `threshold` consecutive failures.
void TripToOpen(CircuitBreaker& cb, int threshold) {
    for (int i = 0; i < threshold; ++i) cb.RecordFailure();
}

TEST(CragBreakerDepthTest, FullRecoveryCycleReClosesAfterCooldown) {
    FakeClock clk;
    CircuitBreaker cb(/*threshold=*/3, /*cooldown_sec=*/10, clk.fn());
    TripToOpen(cb, 3);
    ASSERT_EQ(cb.State(), CircuitBreakerState::kOpen);

    // Cooldown elapses → next AllowRequest yields the half-open probe.
    clk.Advance(10 * 1000);
    ASSERT_TRUE(cb.AllowRequest());
    ASSERT_EQ(cb.State(), CircuitBreakerState::kHalfOpen);

    // Probe succeeds → breaker RE-CLOSES and resumes admitting requests.
    cb.RecordSuccess();
    EXPECT_EQ(cb.State(), CircuitBreakerState::kClosed);
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.failure_count(), 0);
}

TEST(CragBreakerDepthTest, ExactCooldownBoundaryAllowsProbe) {
    FakeClock clk;
    CircuitBreaker cb(/*threshold=*/2, /*cooldown_sec=*/5, clk.fn());
    TripToOpen(cb, 2);
    int64_t opened_at = cb.OpenSinceMs();
    ASSERT_EQ(opened_at, 0);  // tripped at t=0

    // Strictly before cooldown → still open, rejects.
    clk.Advance(5 * 1000 - 1);
    EXPECT_FALSE(cb.AllowRequest());
    EXPECT_EQ(cb.State(), CircuitBreakerState::kOpen);

    // Exactly at cooldown (one more ms reaches the boundary) → half-open probe.
    clk.Advance(1);
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.State(), CircuitBreakerState::kHalfOpen);
}

TEST(CragBreakerDepthTest, HalfOpenFailureReopensAndRestartsCooldown) {
    FakeClock clk;
    CircuitBreaker cb(/*threshold=*/2, /*cooldown_sec=*/10, clk.fn());
    TripToOpen(cb, 2);

    clk.Advance(10 * 1000);
    ASSERT_TRUE(cb.AllowRequest());
    ASSERT_EQ(cb.State(), CircuitBreakerState::kHalfOpen);

    // Probe FAILS → re-open immediately, and the cooldown clock restarts from now.
    cb.RecordFailure();
    EXPECT_EQ(cb.State(), CircuitBreakerState::kOpen);
    EXPECT_EQ(cb.OpenSinceMs(), 10 * 1000);  // re-opened at the half-open instant

    // A partial wait (< cooldown from the RESTART) is not enough.
    clk.Advance(10 * 1000 - 1);
    EXPECT_FALSE(cb.AllowRequest());
    // Completing the restarted cooldown re-admits a probe.
    clk.Advance(1);
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.State(), CircuitBreakerState::kHalfOpen);
}

TEST(CragBreakerDepthTest, MultipleRecoveryCyclesThenReTrip) {
    FakeClock clk;
    CircuitBreaker cb(/*threshold=*/2, /*cooldown_sec=*/3, clk.fn());

    // Cycle 1: trip, recover via probe success.
    TripToOpen(cb, 2);
    clk.Advance(3000);
    ASSERT_TRUE(cb.AllowRequest());
    cb.RecordSuccess();
    ASSERT_EQ(cb.State(), CircuitBreakerState::kClosed);

    // After re-close the failure counter is reset, so it again takes the FULL
    // threshold to trip (a single post-recovery failure must NOT re-open).
    cb.RecordFailure();
    EXPECT_EQ(cb.State(), CircuitBreakerState::kClosed);
    cb.RecordFailure();  // 2nd → trips again
    EXPECT_EQ(cb.State(), CircuitBreakerState::kOpen);
}

TEST(CragBreakerDepthTest, OnStateChangeFiresTransitionSequence) {
    FakeClock clk;
    CircuitBreaker cb(/*threshold=*/2, /*cooldown_sec=*/4, clk.fn());
    std::vector<CircuitBreakerState> seq;
    cb.OnStateChange([&seq](CircuitBreakerState s) { seq.push_back(s); });

    TripToOpen(cb, 2);            // closed -> open
    clk.Advance(4000);
    ASSERT_TRUE(cb.AllowRequest());  // open -> half_open
    cb.RecordSuccess();          // half_open -> closed

    ASSERT_EQ(seq.size(), 3u);
    EXPECT_EQ(seq[0], CircuitBreakerState::kOpen);
    EXPECT_EQ(seq[1], CircuitBreakerState::kHalfOpen);
    EXPECT_EQ(seq[2], CircuitBreakerState::kClosed);
}

TEST(CragBreakerDepthTest, OpenSinceMsIsNegativeWhenNotOpen) {
    FakeClock clk;
    clk.now_ms = 5000;
    CircuitBreaker cb(/*threshold=*/2, /*cooldown_sec=*/4, clk.fn());
    EXPECT_EQ(cb.OpenSinceMs(), -1);  // closed → not open
    TripToOpen(cb, 2);
    EXPECT_EQ(cb.OpenSinceMs(), 5000);  // records the trip instant

    // Recover, then OpenSinceMs goes back to -1.
    clk.Advance(4000);
    ASSERT_TRUE(cb.AllowRequest());
    cb.RecordSuccess();
    EXPECT_EQ(cb.OpenSinceMs(), -1);
}

// ---------------- (2) top1_top5_gap off-by-one boundary ----------------

RankedChunk Chunk(float score) {
    // RankedChunk{child_id, chunk_text, parent_text, score, rerank_score, metadata}
    return RankedChunk{"c", "t", "p", score, 0.0f, {}};
}

CragEvaluator MakeEvaluator() {
    return CragEvaluator(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
}

TEST(CragSignalsDepthTest, FourChunksHaveNoTop1Top5Gap) {
    CragEvaluator ev = MakeEvaluator();
    std::vector<RankedChunk> chunks = {Chunk(0.9f), Chunk(0.7f), Chunk(0.5f), Chunk(0.3f)};
    auto s = ev.ComputeMultiSignals(chunks);
    // size == 4 is strictly below the `>= 5` guard → the gap signal is absent.
    EXPECT_EQ(s.find("top1_top5_gap"), s.end());
    EXPECT_FLOAT_EQ(s["chunk_count"], 4.0f);
}

TEST(CragSignalsDepthTest, FiveChunksHaveExactTop1Top5Gap) {
    CragEvaluator ev = MakeEvaluator();
    std::vector<RankedChunk> chunks = {Chunk(0.9f), Chunk(0.7f), Chunk(0.5f),
                                       Chunk(0.3f), Chunk(0.1f)};
    auto s = ev.ComputeMultiSignals(chunks);
    // size == 5 is exactly the boundary → gap == scores[0] - scores[4] == 0.9-0.1.
    ASSERT_NE(s.find("top1_top5_gap"), s.end());
    EXPECT_NEAR(s["top1_top5_gap"], 0.8f, 1e-5f);
    EXPECT_FLOAT_EQ(s["chunk_count"], 5.0f);
}

TEST(CragSignalsDepthTest, SixChunksGapStillUsesFifthElement) {
    // With > 5 candidates the gap is fixed to scores[0]-scores[4] (NOT the last).
    CragEvaluator ev = MakeEvaluator();
    std::vector<RankedChunk> chunks = {Chunk(1.0f), Chunk(0.8f), Chunk(0.6f),
                                       Chunk(0.4f), Chunk(0.2f), Chunk(0.0f)};
    auto s = ev.ComputeMultiSignals(chunks);
    ASSERT_NE(s.find("top1_top5_gap"), s.end());
    EXPECT_NEAR(s["top1_top5_gap"], 0.8f, 1e-5f);  // 1.0 - 0.2 (5th), not 1.0 - 0.0
}

}  // namespace
}  // namespace cortrix::retrieval
