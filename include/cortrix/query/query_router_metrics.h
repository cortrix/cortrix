#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::query {

/// The `query_router` subsystem metrics (observability naming
/// `cortrix_query_router_<metric>_<unit>`). Mirrors the CragMetrics template
/// (process-wide singleton, atomic counters/histogram/gauge, OpenMetrics renderer).
///
/// 🚨 Cardinality control:
/// labels are enum-only. NO `ns_id` (high-cardinality, forbidden — removed in V3
/// decision 10); per-NS data goes through
/// `GET /api/v1/system/namespaces/<ns_id>/stats` (OBS_SPEC).
///
/// 🚨 standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer. The `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder into that endpoint is cross-Feature wiring
/// **deferred to integration**. Until then it is fully usable + testable in-process and
/// RenderOpenMetrics() produces what the server will serve.
///
/// metric schema (4 rows):
///   cortrix_query_router_total                     counter   {decision}
///   cortrix_query_router_classifier_latency_seconds histogram (no labels)
///   cortrix_query_router_fallback_ratio            gauge     (no labels)
///   cortrix_query_router_compute_saved_seconds     counter   {path}
class QueryRouterMetrics {
public:
    /// decision label for cortrix_query_router_total — the three-tier routing
    /// distribution plus the fail-safe `fallback` bucket (L1/L2/L3 → Complex).
    enum class Decision {
        kSimple = 0,
        kComplex,
        kChat,
        kFallback,
    };

    /// path label for cortrix_query_router_compute_saved_seconds — which
    /// short-path produced the saving vs the full Complex pipeline. `simple` skips
    /// RAG-Fusion, CRAG and HyPE; `chat` skips all retrieval (larger saving).
    enum class SavedPath {
        kSimple = 0,
        kChat,
    };

    /// Process-wide instance (metrics are global counters/histogram/gauge).
    static QueryRouterMetrics& Instance();

    // --- cortrix_query_router_total (Counter, label: decision) ---
    void RecordDecision(Decision decision);
    uint64_t DecisionCount(Decision decision) const;

    // --- cortrix_query_router_classifier_latency_seconds (Histogram, no label) ---
    // Classifier inference latency distribution (rule path P50<0.5ms;
    // LLM path P50<200ms when enabled). Sub-ms..ms buckets straddle the rule SLA.
    void ObserveClassifierLatency(double seconds);
    uint64_t ClassifierLatencyCount() const;
    double ClassifierLatencySum() const;

    // --- cortrix_query_router_fallback_ratio (Gauge, no label) ---
    // L3 fallback ratio (alarm threshold < 5%). Gauge → last write wins.
    void SetFallbackRatio(double ratio);
    double FallbackRatio() const;

    // --- cortrix_query_router_compute_saved_seconds (Counter, label: path) ---
    // Cumulative compute-time saved by short paths vs the full Complex pipeline
    // (monitoring value). Recorded in seconds (accumulated in microseconds).
    void AddComputeSaved(SavedPath path, double seconds);
    double ComputeSavedSeconds(SavedPath path) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/histogram/gauge (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    QueryRouterMetrics() = default;

    static constexpr int kDecisionCount = 4;
    static constexpr int kSavedPathCount = 2;
    // classifier_latency_seconds histogram bucket count (bounds in the .cpp
    // kLatBounds[]; the +1 slot is the implicit +Inf bucket).
    static constexpr int kLatencyBuckets = 8;

    std::array<std::atomic<uint64_t>, kDecisionCount> decision_{};
    // classifier_latency histogram: per-bucket counters (NOT cumulative; Render()
    // accumulates le-wise) + sum-bits (microsecond-precise) + count.
    std::array<std::atomic<uint64_t>, kLatencyBuckets + 1> lat_bkt_{};
    std::atomic<uint64_t> lat_sum_us_{0};  // sum in microseconds (integer-stable)
    std::atomic<uint64_t> lat_count_{0};
    // ratio gauge encoded as fixed-point parts-per-million (atomic double-free).
    std::atomic<int64_t> fallback_ratio_ppm_{0};
    // compute-saved counter accumulated in microseconds per path (integer-stable).
    std::array<std::atomic<uint64_t>, kSavedPathCount> compute_saved_us_{};
};

const char* ToString(QueryRouterMetrics::Decision decision);
const char* ToString(QueryRouterMetrics::SavedPath path);

}  // namespace cortrix::query
