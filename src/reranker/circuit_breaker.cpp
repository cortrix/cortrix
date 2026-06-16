#include <cstdint>
#include "cortrix/reranker/circuit_breaker.h"

#include <chrono>

namespace cortrix::reranker {

namespace {
int64_t DefaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

CircuitBreaker::CircuitBreaker(int threshold, int cooldown_sec, NowFn now_fn)
    : threshold_(threshold > 0 ? threshold : 1),
      cooldown_sec_(cooldown_sec > 0 ? cooldown_sec : 1),
      now_fn_(now_fn ? std::move(now_fn) : NowFn(DefaultNowMs)) {}

bool CircuitBreaker::AllowRequest() {
    CircuitBreakerState s = state_.load(std::memory_order_acquire);
    if (s == CircuitBreakerState::kClosed || s == CircuitBreakerState::kHalfOpen) {
        return true;
    }
    // Open: allow a single probe once cooldown has elapsed (open → half_open).
    int64_t opened = open_since_ms_.load(std::memory_order_relaxed);
    int64_t elapsed_ms = now_fn_() - opened;
    if (opened >= 0 && elapsed_ms >= static_cast<int64_t>(cooldown_sec_) * 1000) {
        CircuitBreakerState expected = CircuitBreakerState::kOpen;
        // CAS so only one thread wins the transition to half_open (single probe).
        if (state_.compare_exchange_strong(expected, CircuitBreakerState::kHalfOpen,
                                           std::memory_order_acq_rel)) {
            if (on_change_) on_change_(CircuitBreakerState::kHalfOpen);
            return true;
        }
        // Lost the race: another thread already moved us out of open.
        CircuitBreakerState now = state_.load(std::memory_order_acquire);
        return now != CircuitBreakerState::kOpen;
    }
    return false;  // still cooling down
}

void CircuitBreaker::RecordSuccess() {
    // A success closes the breaker (from half_open probe) and clears failures.
    CircuitBreakerState prev =
        state_.exchange(CircuitBreakerState::kClosed, std::memory_order_acq_rel);
    failure_count_.store(0, std::memory_order_relaxed);
    open_since_ms_.store(-1, std::memory_order_relaxed);
    if (prev != CircuitBreakerState::kClosed && on_change_) {
        on_change_(CircuitBreakerState::kClosed);
    }
}

void CircuitBreaker::RecordFailure() {
    CircuitBreakerState s = state_.load(std::memory_order_acquire);
    if (s == CircuitBreakerState::kHalfOpen) {
        // A failed probe re-opens immediately (cooldown restarts).
        TripOpen();
        return;
    }
    if (s == CircuitBreakerState::kOpen) {
        return;  // already open; nothing to count
    }
    // closed: count consecutive failures, trip at threshold.
    int count = failure_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count >= threshold_) {
        TripOpen();
    }
}

void CircuitBreaker::TripOpen() {
    open_since_ms_.store(now_fn_(), std::memory_order_relaxed);
    state_.store(CircuitBreakerState::kOpen, std::memory_order_release);
    if (on_change_) on_change_(CircuitBreakerState::kOpen);
}

int64_t CircuitBreaker::OpenSinceMs() const {
    if (state_.load(std::memory_order_acquire) == CircuitBreakerState::kClosed) {
        return -1;
    }
    return open_since_ms_.load(std::memory_order_relaxed);
}

}  // namespace cortrix::reranker
