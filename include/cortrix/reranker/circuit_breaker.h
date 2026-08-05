#pragma once
#include <atomic>
#include <cstdint>
#include <functional>

namespace cortrix::reranker {

/// Circuit breaker states. Underlying int values double as the
/// cortrix_reranker_circuit_breaker_state gauge values (0/1/2).
enum class CircuitBreakerState : int {
    kClosed = 0,    ///< normal: requests allowed
    kOpen = 1,      ///< tripped: requests rejected for cooldown_sec
    kHalfOpen = 2,  ///< probing: one trial request allowed
};

/// CircuitBreaker — reranker systemic-failure breaker (F02 §3.2 / §4.3 / topic
/// 1.3 / 3.4). State machine:
///   closed --(threshold consecutive failures)--> open
///   open   --(cooldown_sec elapsed, next AllowRequest)--> half_open
///   half_open --(success)--> closed
///   half_open --(failure)--> open (cooldown restarts)
///
/// Thread-safe (atomics): AllowRequest()/RecordSuccess()/RecordFailure() may be
/// called concurrently from worker + caller threads. The open→half_open
/// transition is evaluated lazily in AllowRequest() against an injectable clock
/// (so tests need not sleep cooldown_sec).
class CircuitBreaker {
public:
    using NowFn = std::function<int64_t()>;  ///< returns "now" in epoch milliseconds

    /// @param threshold     consecutive failures that trip closed → open
    /// @param cooldown_sec  open dwell before a half-open probe is allowed
    /// @param now_fn        clock (epoch ms); default = steady wall clock. Tests
    ///                      inject a controllable clock.
    CircuitBreaker(int threshold, int cooldown_sec, NowFn now_fn = {});

    /// True if a request may proceed (closed / half_open). When open and the
    /// cooldown has elapsed, transitions open → half_open and returns true (the
    /// single probe). Has side effects (the lazy open→half_open transition).
    bool AllowRequest();

    /// Record a successful rerank: half_open → closed (resets failures); closed
    /// resets the consecutive-failure counter.
    void RecordSuccess();

    /// Record a failed rerank: in closed, increments the counter and trips to
    /// open at `threshold`; in half_open, a failure re-opens immediately.
    void RecordFailure();

    /// Register a callback fired on every state transition, with the new state
    /// (S2.5 — drives the circuit_breaker_state gauge + trips counter + WARN/INFO
    /// log). Single callback; set once at wiring time (not thread-safe to mutate
    /// concurrently with transitions). Fired while no internal lock is held.
    void OnStateChange(std::function<void(CircuitBreakerState)> cb) {
        on_change_ = std::move(cb);
    }

    CircuitBreakerState State() const {
        return state_.load(std::memory_order_acquire);
    }

    /// Epoch-ms timestamp the breaker last entered open, or -1 if not open.
    int64_t OpenSinceMs() const;

    int failure_count() const { return failure_count_.load(std::memory_order_relaxed); }
    int threshold() const { return threshold_; }
    int cooldown_sec() const { return cooldown_sec_; }

private:
    void TripOpen();

    std::atomic<CircuitBreakerState> state_{CircuitBreakerState::kClosed};
    std::atomic<int> failure_count_{0};
    std::atomic<int64_t> open_since_ms_{-1};
    const int threshold_;
    const int cooldown_sec_;
    NowFn now_fn_;
    std::function<void(CircuitBreakerState)> on_change_;
};

}  // namespace cortrix::reranker
