// S2.4 — CircuitBreaker 3-state machine + transitions (closed/open/half_open).
#include <gtest/gtest.h>

#include <cstdint>

#include "cortrix/reranker/circuit_breaker.h"

namespace cortrix::reranker {
namespace {

// Controllable clock so cooldown transitions don't need real sleeps.
struct FakeClock {
    int64_t now_ms = 0;
    CircuitBreaker::NowFn fn() {
        return [this] { return now_ms; };
    }
    void Advance(int64_t ms) { now_ms += ms; }
};

TEST(CircuitBreakerTest, StartsClosedAndAllowsRequests) {
    CircuitBreaker cb(10, 30);
    EXPECT_EQ(cb.State(), CircuitBreakerState::kClosed);
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.OpenSinceMs(), -1);
}

TEST(CircuitBreakerTest, NinthFailureDoesNotTripTenthDoes) {
    CircuitBreaker cb(10, 30);
    for (int i = 0; i < 9; ++i) cb.RecordFailure();
    EXPECT_EQ(cb.State(), CircuitBreakerState::kClosed);  // 9 < threshold
    EXPECT_TRUE(cb.AllowRequest());
    cb.RecordFailure();  // 10th
    EXPECT_EQ(cb.State(), CircuitBreakerState::kOpen);
    EXPECT_FALSE(cb.AllowRequest());  // open rejects
    EXPECT_GE(cb.OpenSinceMs(), 0);
}

TEST(CircuitBreakerTest, SuccessResetsConsecutiveFailureCount) {
    CircuitBreaker cb(10, 30);
    for (int i = 0; i < 9; ++i) cb.RecordFailure();
    cb.RecordSuccess();  // resets the counter
    EXPECT_EQ(cb.failure_count(), 0);
    for (int i = 0; i < 9; ++i) cb.RecordFailure();
    EXPECT_EQ(cb.State(), CircuitBreakerState::kClosed);  // only 9 again → no trip
}

TEST(CircuitBreakerTest, OpenTransitionsToHalfOpenAfterCooldown) {
    FakeClock clk;
    CircuitBreaker cb(3, /*cooldown_sec=*/30, clk.fn());
    for (int i = 0; i < 3; ++i) cb.RecordFailure();
    ASSERT_EQ(cb.State(), CircuitBreakerState::kOpen);

    // Before cooldown elapses → still open, rejects.
    clk.Advance(29'000);
    EXPECT_FALSE(cb.AllowRequest());
    EXPECT_EQ(cb.State(), CircuitBreakerState::kOpen);

    // After cooldown (30s) → next AllowRequest yields the half-open probe.
    clk.Advance(1'000);
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.State(), CircuitBreakerState::kHalfOpen);
}

TEST(CircuitBreakerTest, HalfOpenSuccessClosesBreaker) {
    FakeClock clk;
    CircuitBreaker cb(3, 30, clk.fn());
    for (int i = 0; i < 3; ++i) cb.RecordFailure();
    clk.Advance(30'000);
    ASSERT_TRUE(cb.AllowRequest());
    ASSERT_EQ(cb.State(), CircuitBreakerState::kHalfOpen);

    cb.RecordSuccess();  // probe succeeded
    EXPECT_EQ(cb.State(), CircuitBreakerState::kClosed);
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.OpenSinceMs(), -1);
}

TEST(CircuitBreakerTest, HalfOpenFailureReopens) {
    FakeClock clk;
    CircuitBreaker cb(3, 30, clk.fn());
    for (int i = 0; i < 3; ++i) cb.RecordFailure();
    clk.Advance(30'000);
    ASSERT_TRUE(cb.AllowRequest());
    ASSERT_EQ(cb.State(), CircuitBreakerState::kHalfOpen);
    int64_t first_open = cb.OpenSinceMs();

    clk.Advance(5'000);
    cb.RecordFailure();  // probe failed → re-open, cooldown restarts
    EXPECT_EQ(cb.State(), CircuitBreakerState::kOpen);
    EXPECT_FALSE(cb.AllowRequest());
    EXPECT_GT(cb.OpenSinceMs(), first_open);  // open timestamp refreshed
}

TEST(CircuitBreakerTest, ZeroThresholdClampedToOne) {
    CircuitBreaker cb(0, 0);  // degenerate → clamped to threshold 1 / cooldown 1
    EXPECT_EQ(cb.threshold(), 1);
    EXPECT_EQ(cb.cooldown_sec(), 1);
    cb.RecordFailure();  // single failure trips (threshold 1)
    EXPECT_EQ(cb.State(), CircuitBreakerState::kOpen);
}

}  // namespace
}  // namespace cortrix::reranker
