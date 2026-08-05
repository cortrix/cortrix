#pragma once
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/reranker/circuit_breaker.h"

namespace cortrix::reranker {

/// Reranker health/status reporting.
///
/// Bundles the three concerns the breaker's state change drives:
///   1. metric: cortrix_reranker_circuit_breaker_state gauge (0/1/2) +
///      _trips_total counter (incremented on each closed/half→open trip);
///   2. structured status: the pgcortrix_status() reranker_status JSONB (§3.5);
///   3. log: WARN on open, INFO on close (§3.4 server-log spec).
///
/// Standalone (D3): builds the JSONB + records to the in-process RerankerMetrics
/// + logs. Exposing it through the live pgcortrix_status() SQL function + the
/// /metrics endpoint is cross-Feature wiring → D3.5.
class RerankerStatusReporter {
public:
    /// Format an epoch-ms timestamp as an ISO-8601 UTC string ("…Z"). Returns ""
    /// (JSON null upstream) for a negative (not-open) timestamp.
    static std::string ToIso8601Utc(int64_t epoch_ms);

    /// Build the §3.5 pgcortrix_status() reranker fields:
    ///   { reranker_status: "ok"|"degraded"|"circuit_open",
    ///     circuit_breaker_open_since: ISO-8601 | null,
    ///     last_failure_reason: string | null,
    ///     active_ep: "coreml"|"cpu" }
    /// status mapping: open → "circuit_open"; half_open → "degraded"; closed →
    /// "ok".
    static nlohmann::json BuildStatusJson(CircuitBreakerState state,
                                          int64_t open_since_ms,
                                          const std::string& last_failure_reason,
                                          const std::string& active_ep);

    /// Transition handler to wire via CircuitBreaker::OnStateChange. Updates the
    /// circuit_breaker_state gauge, bumps trips_total on entry to open, and logs
    /// WARN (open) / INFO (closed). cooldown_sec is included in the WARN message
    /// per §3.4 ("fallback RRF for 30s").
    static void OnCircuitStateChange(CircuitBreakerState new_state, int cooldown_sec);

    // --- Alert rules (ops-side Prometheus) ---
    // SoT for the two reranker alert rule expressions; surfaced so the ops alert
    // bundle (D3.5) can emit them verbatim.
    static std::string AlertRuleCircuitOpen5m();   ///< state==1 for >= 5m
    static std::string AlertRuleFrequentTrips1h();  ///< trips increase >= 3 in 1h
};

}  // namespace cortrix::reranker
