#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::retrieval {

/// The `crag` subsystem metrics (F37 §10, OBSERVABILITY_SPEC naming
/// `cortrix_crag_<metric>_<unit>`). Mirrors the F36 RagFusionMetrics template
/// (process-wide singleton, atomic counters/histogram/gauges, OpenMetrics
/// renderer).
///
/// 🚨 Cardinality control (OBSERVABILITY_SPEC §3.2 — F37 §10 / v1.0.3 qa-tier2-g3
/// M3): labels are enum-only. NO `ns_id` (high-cardinality, forbidden — removed in
/// D1 V3 decision 10); per-NS data goes through
/// `GET /api/v1/system/namespaces/<ns_id>/stats` (OBS_SPEC §3.4).
///
/// 🚨 D3 standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer. The F24 `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder into that endpoint is cross-Feature wiring
/// **deferred to D3.5**. Until then it is fully usable + testable in-process and
/// RenderOpenMetrics() produces what F24 will serve.
///
/// §10 metric schema (4 rows):
///   cortrix_crag_evaluation_total            counter   {decision}
///   cortrix_crag_classifier_latency_seconds  histogram (no labels)
///   cortrix_crag_fallback_ratio              gauge     (no labels)
///   cortrix_crag_incorrect_ratio             gauge     (no labels)
class CragMetrics {
public:
    /// decision label for cortrix_crag_evaluation_total (§10). `fallback` = the L3
    /// transparent-degrade verdict ("correct_fallback_classifier_failed").
    enum class Decision {
        kCorrect = 0,
        kAmbiguous,
        kIncorrect,
        kFallback,
    };

    /// Process-wide instance (metrics are global counters/histogram/gauges).
    static CragMetrics& Instance();

    // --- cortrix_crag_evaluation_total (Counter, label: decision) ---
    void RecordEvaluation(Decision decision);
    uint64_t EvaluationCount(Decision decision) const;

    // --- cortrix_crag_classifier_latency_seconds (Histogram, no label) ---
    // Classifier inference latency distribution (F37 §12.bis 2.1: P50<5ms / P99<20ms).
    void ObserveClassifierLatency(double seconds);
    uint64_t ClassifierLatencyCount() const;
    double ClassifierLatencySum() const;

    // --- cortrix_crag_fallback_ratio (Gauge, no label) ---
    // L3 fallback ratio (F37 §12.bis 2.2 alarm threshold). Gauge → last write wins.
    void SetFallbackRatio(double ratio);
    double FallbackRatio() const;

    // --- cortrix_crag_incorrect_ratio (Gauge, no label) ---
    // Incorrect-verdict ratio (§10 — business alarm threshold). Gauge.
    void SetIncorrectRatio(double ratio);
    double IncorrectRatio() const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauges (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    CragMetrics() = default;

    static constexpr int kDecisionCount = 4;
    // classifier_latency_seconds histogram bucket count (bounds in the .cpp
    // kLatBounds[]; the +1 slot is the implicit +Inf bucket).
    static constexpr int kLatencyBuckets = 8;

    std::array<std::atomic<uint64_t>, kDecisionCount> evaluation_{};
    // classifier_latency histogram: per-bucket counters (NOT cumulative; Render()
    // accumulates le-wise) + sum-bits (CAS double, microsecond-precise) + count.
    std::array<std::atomic<uint64_t>, kLatencyBuckets + 1> lat_bkt_{};
    std::atomic<uint64_t> lat_sum_us_{0};  // sum in microseconds (integer-stable)
    std::atomic<uint64_t> lat_count_{0};
    // ratio gauges encoded as fixed-point parts-per-million (atomic double-free).
    std::atomic<int64_t> fallback_ratio_ppm_{0};
    std::atomic<int64_t> incorrect_ratio_ppm_{0};
};

const char* ToString(CragMetrics::Decision decision);

}  // namespace cortrix::retrieval
