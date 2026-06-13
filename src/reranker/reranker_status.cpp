#include "cortrix/reranker/reranker_status.h"

#include <ctime>

#include <spdlog/spdlog.h>

#include "cortrix/reranker/reranker_metrics.h"

namespace cortrix::reranker {

std::string RerankerStatusReporter::ToIso8601Utc(int64_t epoch_ms) {
    if (epoch_ms < 0) return "";
    std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

nlohmann::json RerankerStatusReporter::BuildStatusJson(CircuitBreakerState state,
                                                       int64_t open_since_ms,
                                                       const std::string& last_failure_reason,
                                                       const std::string& active_ep) {
    nlohmann::json j;
    switch (state) {
        case CircuitBreakerState::kClosed:   j["reranker_status"] = "ok"; break;
        case CircuitBreakerState::kHalfOpen: j["reranker_status"] = "degraded"; break;
        case CircuitBreakerState::kOpen:     j["reranker_status"] = "circuit_open"; break;
    }
    if (state == CircuitBreakerState::kClosed || open_since_ms < 0) {
        j["circuit_breaker_open_since"] = nullptr;
    } else {
        j["circuit_breaker_open_since"] = ToIso8601Utc(open_since_ms);
    }
    if (last_failure_reason.empty()) {
        j["last_failure_reason"] = nullptr;
    } else {
        j["last_failure_reason"] = last_failure_reason;
    }
    j["active_ep"] = active_ep;
    return j;
}

void RerankerStatusReporter::OnCircuitStateChange(CircuitBreakerState new_state,
                                                  int cooldown_sec) {
    auto& m = RerankerMetrics::Instance();
    m.SetCircuitBreakerState(static_cast<int>(new_state));
    switch (new_state) {
        case CircuitBreakerState::kOpen:
            m.RecordCircuitBreakerTrip();
            spdlog::warn(
                "reranker circuit breaker OPEN, fallback RRF for {}s, "
                "reason: consecutive reranker task failures", cooldown_sec);
            break;
        case CircuitBreakerState::kClosed:
            spdlog::info("reranker circuit breaker CLOSED, normal operation resumed");
            break;
        case CircuitBreakerState::kHalfOpen:
            // Half-open is a transient probe; no WARN/INFO per §3.4 (only the gauge
            // moves to 2). Kept explicit so -Wswitch covers the enum.
            break;
    }
}

std::string RerankerStatusReporter::AlertRuleCircuitOpen5m() {
    // §3.4: circuit_breaker_state == 1 sustained >= 5 minutes → systemic failure.
    return "cortrix_reranker_circuit_breaker_state == 1 for 5m";
}

std::string RerankerStatusReporter::AlertRuleFrequentTrips1h() {
    // §3.4: trips_total increases >= 3 within 1h → instability.
    return "increase(cortrix_reranker_circuit_breaker_trips_total[1h]) >= 3";
}

}  // namespace cortrix::reranker
