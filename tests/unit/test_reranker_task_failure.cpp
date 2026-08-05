// S3.4 — single-task exception handling (ONNX exception / OOM / timeout) →
// score=0 + cortrix_reranker_failed_tasks_total{reason} + circuit-breaker
// linkage (reranker / §5.1 / Issue 1.3 / 3.4). Failures are injected via a test
// subclass overriding the protected ScoreForTask hook.
#include <gtest/gtest.h>

#include <chrono>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/reranker/onnx_reranker.h"
#include "cortrix/reranker/reranker_metrics.h"

namespace cortrix::reranker {
namespace {

using FailedTaskReason = RerankerMetrics::FailedTaskReason;

// Always throws an ONNX-style exception from the per-task inference hook.
class ThrowingReranker : public OnnxReranker {
public:
    using OnnxReranker::OnnxReranker;
    // Quiesce workers before this dtor demotes the vptr (header contract).
    ~ThrowingReranker() override { Shutdown(); }

protected:
    float ScoreForTask(const char*, const char*) override {
        throw std::runtime_error("simulated ONNX inference failure");
    }
};

// Throws std::bad_alloc (OOM) from the per-task inference hook.
class OomReranker : public OnnxReranker {
public:
    using OnnxReranker::OnnxReranker;
    ~OomReranker() override { Shutdown(); }

protected:
    float ScoreForTask(const char*, const char*) override { throw std::bad_alloc(); }
};

// Sleeps past the task timeout in the per-task hook.
class SlowReranker : public OnnxReranker {
public:
    using OnnxReranker::OnnxReranker;
    // The timed-out task is still sleeping on a worker when the test ends —
    // join it before vptr demotion (this was the TSAN-caught vptr race).
    ~SlowReranker() override { Shutdown(); }

protected:
    float ScoreForTask(const char*, const char*) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return 0.9f;
    }
};

RerankerConfig BreakerConfig(int threshold) {
    RerankerConfig c;  // stub mode (empty model_path)
    c.circuit_breaker_threshold = threshold;
    c.circuit_breaker_cooldown_sec = 30;
    return c;
}

std::vector<const char*> Passages(const std::vector<std::string>& v) {
    std::vector<const char*> out;
    out.reserve(v.size());
    for (const auto& s : v) out.push_back(s.c_str());
    return out;
}

// --- ONNX exception → score=0 + onnx_exception metric ---

TEST(RerankerTaskFailureTest, OnnxExceptionScoresZeroAndRecordsMetric) {
    RerankerMetrics::Instance().ResetForTest();
    ThrowingReranker r(BreakerConfig(10), nullptr);
    ASSERT_TRUE(r.Init().ok());

    std::vector<std::string> texts{"a", "b"};
    auto p = Passages(texts);
    std::vector<float> scores = r.ScoreBatch("q", p);

    ASSERT_EQ(scores.size(), 2u);
    EXPECT_EQ(scores[0], 0.0f);
    EXPECT_EQ(scores[1], 0.0f);
    EXPECT_EQ(RerankerMetrics::Instance().FailedTasksCount(FailedTaskReason::kOnnxException), 2u);
}

// --- OOM → score=0 + oom metric ---

TEST(RerankerTaskFailureTest, OomScoresZeroAndRecordsOomMetric) {
    RerankerMetrics::Instance().ResetForTest();
    OomReranker r(BreakerConfig(10), nullptr);
    ASSERT_TRUE(r.Init().ok());

    std::vector<std::string> texts{"a"};
    auto p = Passages(texts);
    std::vector<float> scores = r.ScoreBatch("q", p);

    ASSERT_EQ(scores.size(), 1u);
    EXPECT_EQ(scores[0], 0.0f);
    EXPECT_EQ(RerankerMetrics::Instance().FailedTasksCount(FailedTaskReason::kOom), 1u);
}

// --- timeout → score=0 + timeout metric ---

TEST(RerankerTaskFailureTest, TimeoutScoresZeroAndRecordsTimeoutMetric) {
    RerankerMetrics::Instance().ResetForTest();
    RerankerConfig c = BreakerConfig(10);
    c.task_timeout_ms = 10;  // far below the 120ms ScoreForTask sleep
    SlowReranker r(c, nullptr);
    ASSERT_TRUE(r.Init().ok());

    std::vector<std::string> texts{"slow"};
    auto p = Passages(texts);
    std::vector<float> scores = r.ScoreBatch("q", p);

    ASSERT_EQ(scores.size(), 1u);
    EXPECT_EQ(scores[0], 0.0f);
    EXPECT_EQ(RerankerMetrics::Instance().FailedTasksCount(FailedTaskReason::kTimeout), 1u);
}

// --- consecutive failures trip the breaker (closed → open) ---

TEST(RerankerTaskFailureTest, ConsecutiveFailuresTripBreaker) {
    RerankerMetrics::Instance().ResetForTest();
    ThrowingReranker r(BreakerConfig(3), nullptr);  // trips after 3 consecutive
    ASSERT_TRUE(r.Init().ok());
    EXPECT_EQ(r.circuit_state(), CircuitBreakerState::kClosed);

    // 3 failing passages in one batch → 3 consecutive RecordFailure → OPEN.
    std::vector<std::string> texts{"a", "b", "c"};
    auto p = Passages(texts);
    r.ScoreBatch("q", p);

    EXPECT_EQ(r.circuit_state(), CircuitBreakerState::kOpen);
    EXPECT_GE(RerankerMetrics::Instance().CircuitBreakerTripsCount(), 1u);
}

// --- once open, ScoreBatch short-circuits to all-zeros (RRF fallback) ---

TEST(RerankerTaskFailureTest, OpenBreakerReturnsAllZerosWithoutScoring) {
    RerankerMetrics::Instance().ResetForTest();
    ThrowingReranker r(BreakerConfig(2), nullptr);
    ASSERT_TRUE(r.Init().ok());

    // Trip it.
    std::vector<std::string> trip{"a", "b"};
    auto pt = Passages(trip);
    r.ScoreBatch("q", pt);
    ASSERT_EQ(r.circuit_state(), CircuitBreakerState::kOpen);

    uint64_t failed_before =
        RerankerMetrics::Instance().FailedTasksCount(FailedTaskReason::kOnnxException);

    // Next batch while OPEN: returns all zeros and does NOT invoke inference (so
    // the onnx_exception counter must not advance).
    std::vector<std::string> texts{"x", "y", "z"};
    auto p = Passages(texts);
    std::vector<float> scores = r.ScoreBatch("q", p);
    ASSERT_EQ(scores.size(), 3u);
    for (float s : scores) EXPECT_EQ(s, 0.0f);
    EXPECT_EQ(RerankerMetrics::Instance().FailedTasksCount(FailedTaskReason::kOnnxException),
              failed_before);
}

// --- a success resets the consecutive-failure count (no premature trip) ---

TEST(RerankerTaskFailureTest, SuccessResetsConsecutiveCount) {
    RerankerMetrics::Instance().ResetForTest();
    // Default OnnxReranker (stub mode) always succeeds → breaker stays closed.
    OnnxReranker r(BreakerConfig(3), nullptr);
    ASSERT_TRUE(r.Init().ok());
    std::vector<std::string> texts{"a", "b", "c", "d", "e"};
    auto p = Passages(texts);
    std::vector<float> scores = r.ScoreBatch("q", p);
    ASSERT_EQ(scores.size(), 5u);
    EXPECT_EQ(r.circuit_state(), CircuitBreakerState::kClosed);
    EXPECT_EQ(RerankerMetrics::Instance().CircuitBreakerTripsCount(), 0u);
}

// --- EXTREMELY_LONG (data quality) does NOT count as a task failure ---

TEST(RerankerTaskFailureTest, ExtremelyLongDoesNotTripBreaker) {
    RerankerMetrics::Instance().ResetForTest();
    OnnxReranker r(BreakerConfig(2), nullptr);  // would trip after 2 real failures
    ASSERT_TRUE(r.Init().ok());

    // Build 3 extremely-long passages (> 4×512 tokens). They force score=0 as a
    // data-quality fallback but must NOT advance the breaker / failed_tasks.
    std::string huge(4 * 512 * 4 + 4096, 'x');  // bytes → well over 2048 tokens
    std::vector<std::string> texts{huge, huge, huge};
    auto p = Passages(texts);
    std::vector<float> scores = r.ScoreBatch("q", p);

    ASSERT_EQ(scores.size(), 3u);
    for (float s : scores) EXPECT_EQ(s, 0.0f);
    EXPECT_EQ(r.circuit_state(), CircuitBreakerState::kClosed);
    EXPECT_EQ(RerankerMetrics::Instance().ExtremelyLongCount(), 3u);
    EXPECT_EQ(RerankerMetrics::Instance().FailedTasksCount(FailedTaskReason::kOnnxException), 0u);
}

}  // namespace
}  // namespace cortrix::reranker
