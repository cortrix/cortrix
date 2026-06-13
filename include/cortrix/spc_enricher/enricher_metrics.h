#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "cortrix/spc_enricher/enricher_error.h"

namespace cortrix::spc {

/// The enricher subsystem metrics (F03 §3.6, OBSERVABILITY_SPEC subsystem
/// `enricher`). Naming `cortrix_enricher_<metric>_<unit>` (V2 ruling #3: cortrix_
/// prefix, no plugin prefix; V12 P0 CRIT-2 dropped the high-cardinality ns_id
/// label from tokens_total / cost_usd_total).
///
/// Standalone (D3): a self-contained, dependency-free recorder + OpenMetrics text
/// renderer (same pattern as RerankerMetrics / OnnxMetrics). The F24 `/metrics`
/// scrape endpoint does not exist in the frozen tree — registering this recorder
/// into that endpoint is cross-Feature wiring deferred to D3.5. Until then it is
/// fully usable + testable in-process and Render() produces what F24 will serve.
class EnricherMetrics {
public:
    /// Reason label for cortrix_enricher_fallback_to_null_total (§4.4 scenarios).
    enum class FallbackReason {
        kApiKeyMissing = 0,        ///< enricher.type=llm but api_key unset
        kEndpointUnreachable,      ///< startup endpoint probe failed
        kBudgetExceeded,           ///< budget cap hit → degrade
        kCircuitOpen,              ///< breaker open → degrade
    };

    /// Process-wide instance (metrics are global counters/gauges).
    static EnricherMetrics& Instance();

    // --- cortrix_enricher_circuit_breaker_state (Gauge: 0/1/2) — S3.4 ---
    void SetCircuitBreakerState(int state);
    int CircuitBreakerState() const;

    // --- cortrix_enricher_circuit_breaker_trips_total (Counter) — S3.4 ---
    void RecordCircuitBreakerTrip();
    uint64_t CircuitBreakerTripsCount() const;

    // --- cortrix_enricher_failed_tasks_total (Counter, label: reason=6 codes) — S3.3 ---
    void RecordFailedTask(EnricherErrorCode reason);
    uint64_t FailedTasksCount(EnricherErrorCode reason) const;

    // --- cortrix_enricher_fallback_to_null_total (Counter, label: reason) — S3.4/W4 ---
    void RecordFallbackToNull(FallbackReason reason);
    uint64_t FallbackToNullCount(FallbackReason reason) const;

    // --- cortrix_enricher_endpoint_probe_failed_total (Counter) — W4 ---
    void RecordEndpointProbeFailed();
    uint64_t EndpointProbeFailedCount() const;

    // --- cortrix_enricher_tokens_total (Counter, label: model) — S3.4 ---
    void RecordTokens(const std::string& model, int64_t tokens);
    int64_t TokensCount(const std::string& model) const;

    // --- cortrix_enricher_cost_usd_total (Counter, label: model) — S4.1 ---
    // Stored as micro-USD to stay integral; rendered as USD.
    void RecordCostMicroUsd(const std::string& model, int64_t micro_usd);
    int64_t CostMicroUsd(const std::string& model) const;

    // --- cortrix_enricher_truncated_total (Counter) — S3.3 ---
    void RecordTruncated();
    uint64_t TruncatedCount() const;

    // --- cortrix_enricher_queue_depth_current (Gauge) — S3.1 ---
    void SetQueueDepth(int depth);
    int QueueDepth() const;

    // --- cortrix_enricher_batch_size_actual (Histogram, §3.6) — actual per-LLM-call
    // batch size (topic 1.2 batch_size 1-32, default 8); fed once per RunOneBatch. ---
    void ObserveBatchSizeActual(int batch_size);
    uint64_t BatchSizeActualCount() const;

    // --- cortrix_enricher_score_duration_seconds (Histogram, §3.6) — EnrichBatch
    // per-LLM-call latency distribution (§4.2 "update metric ... duration"). ---
    void ObserveScoreDuration(double seconds);
    uint64_t ScoreDurationCount() const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauges (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    EnricherMetrics() = default;

    static constexpr int kFallbackReasonCount = 4;

    // Histogram bucket counts (bounds + rendering live in the .cpp). batch_size_actual
    // is a count distribution over topic-1.2's 1-32 range; score_duration is seconds.
    static constexpr int kBatchBucketCount = 6;  // {1,2,4,8,16,32}
    static constexpr int kDurBucketCount = 8;    // {0.1,0.25,0.5,1,2,5,10,30}s

    std::atomic<int> cb_state_{0};                    // 0=closed default
    std::atomic<uint64_t> cb_trips_{0};
    std::array<std::atomic<uint64_t>, kEnricherErrorCodeCount> failed_tasks_{};
    std::array<std::atomic<uint64_t>, kFallbackReasonCount> fallback_to_null_{};
    std::atomic<uint64_t> endpoint_probe_failed_{0};
    std::atomic<uint64_t> truncated_{0};
    std::atomic<int> queue_depth_{0};

    // batch_size_actual histogram (last bucket = +Inf). sum kept as integer total.
    std::array<std::atomic<uint64_t>, kBatchBucketCount + 1> batch_bkt_{};
    std::atomic<uint64_t> batch_sum_{0};
    std::atomic<uint64_t> batch_count_{0};

    // score_duration_seconds histogram (last bucket = +Inf). sum kept in micro-seconds
    // (integral) → rendered as seconds (same trick as scoring_metrics.cpp Hist::sum_us).
    std::array<std::atomic<uint64_t>, kDurBucketCount + 1> dur_bkt_{};
    std::atomic<uint64_t> dur_sum_us_{0};
    std::atomic<uint64_t> dur_count_{0};

    // Per-model counters (model is a bounded-cardinality label). Guarded by mu_.
    mutable std::mutex mu_;
    std::map<std::string, int64_t> tokens_by_model_;
    std::map<std::string, int64_t> cost_micro_usd_by_model_;
};

const char* ToString(EnricherMetrics::FallbackReason reason);

}  // namespace cortrix::spc
