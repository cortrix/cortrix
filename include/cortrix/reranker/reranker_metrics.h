#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::reranker {

/// The reranker subsystem metrics (F02 §3.4, OBSERVABILITY_SPEC subsystem
/// `reranker`). Naming `cortrix_reranker_<metric>_<unit>` (V2 ruling #3: cortrix_
/// prefix, no plugin prefix).
///
/// Standalone (D3): a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer (same pattern as F22 OnnxMetrics). The F24 `/metrics` scrape
/// endpoint does not exist in the frozen tree — registering this recorder into
/// that endpoint is cross-Feature wiring deferred to D3.5. Until then it is fully
/// usable + testable in-process and Render() produces what F24 will serve.
///
/// This class is introduced in S1.4 (EP metrics) and extended in S2.5 (circuit-
/// breaker metrics); later Waves fill the input-preprocessing counters.
class RerankerMetrics {
public:
    /// reason label for cortrix_reranker_coreml_fallback_total.
    enum class CoremlFallbackReason {
        kEpInitFailed = 0,  ///< AppendExecutionProvider("CoreML") threw
        kUnsupportedPlatform,  ///< non-Apple / no CoreML compiled in
    };

    /// reason label for cortrix_reranker_failed_tasks_total (S3.4 / S2.5).
    enum class FailedTaskReason {
        kOnnxException = 0,
        kOom,
        kTimeout,
    };

    /// Process-wide instance (metrics are global counters/gauges).
    static RerankerMetrics& Instance();

    // --- cortrix_reranker_coreml_fallback_total (Counter, label: reason) — S1.4 ---
    void RecordCoremlFallback(CoremlFallbackReason reason);
    uint64_t CoremlFallbackCount(CoremlFallbackReason reason) const;

    // --- cortrix_reranker_active_ep (Gauge, label: ep="coreml"|"cpu") — S1.4 ---
    void SetActiveEp(const std::string& ep);
    std::string ActiveEp() const;

    // --- cortrix_reranker_circuit_breaker_state (Gauge: 0/1/2) — S2.5 ---
    void SetCircuitBreakerState(int state);
    int CircuitBreakerState() const;

    // --- cortrix_reranker_circuit_breaker_trips_total (Counter) — S2.5 ---
    void RecordCircuitBreakerTrip();
    uint64_t CircuitBreakerTripsCount() const;

    // --- cortrix_reranker_failed_tasks_total (Counter, label: reason) — S2.5/S3.4 ---
    void RecordFailedTask(FailedTaskReason reason);
    uint64_t FailedTasksCount(FailedTaskReason reason) const;

    // --- cortrix_reranker_truncated_total (Counter, no label) — S3.3 ---
    // Incremented once per passage smart-truncated (TRUNCATED category).
    void RecordTruncated();
    uint64_t TruncatedCount() const;

    // --- cortrix_reranker_extremely_long_total (Counter, no label) — S3.3 ---
    // Incremented once per passage forced to score=0 (EXTREMELY_LONG category).
    void RecordExtremelyLong();
    uint64_t ExtremelyLongCount() const;

    // --- cortrix_reranker_queue_depth_current (Gauge, no label) — D35-MET-04 ---
    // Current ThreadPool queue depth (tasks queued, not yet started). Sampled from
    // RerankerThreadPool::QueueDepth() inside ScoreBatch. A gauge → last write wins.
    void SetQueueDepth(int depth);
    int QueueDepth() const;

    // --- cortrix_reranker_score_duration_seconds (Histogram, no label) — D35-MET-04 ---
    // ScoreBatch end-to-end latency distribution (F02 §3.4). Bucket bounds straddle
    // the F02 SLA (component P99=200ms; 30-candidate batch ~240ms; e2e P50<500ms /
    // P99<1500ms) — see kScoreBounds in the .cpp.
    void ObserveScoreDuration(double seconds);
    uint64_t ScoreDurationCount() const;
    double ScoreDurationSum() const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauges (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    RerankerMetrics() = default;

    static constexpr int kCoremlReasonCount = 2;
    static constexpr int kFailedReasonCount = 3;
    // score_duration_seconds histogram bucket count (upper bounds in the .cpp
    // kScoreBounds[]; the +1 slot is the implicit +Inf bucket).
    static constexpr int kScoreBucketCount = 8;

    std::array<std::atomic<uint64_t>, kCoremlReasonCount> coreml_fallback_{};
    std::array<std::atomic<uint64_t>, kFailedReasonCount> failed_tasks_{};
    std::atomic<int> cb_state_{0};               // 0=closed (default)
    std::atomic<uint64_t> cb_trips_{0};
    std::atomic<uint64_t> truncated_{0};         // S3.3
    std::atomic<uint64_t> extremely_long_{0};    // S3.3
    // active_ep gauge: encoded as 0=cpu / 1=coreml (string rendered in Render()).
    std::atomic<int> active_ep_coreml_{0};
    // queue_depth_current gauge (D35-MET-04): last sampled ThreadPool depth.
    std::atomic<int> queue_depth_{0};
    // score_duration_seconds histogram (D35-MET-04): per-bucket counters (NOT
    // cumulative; Render() accumulates le-wise), + sum-bits (CAS double) + count.
    std::array<std::atomic<uint64_t>, kScoreBucketCount + 1> score_dur_bkt_{};
    std::atomic<uint64_t> score_dur_sum_bits_{0};
    std::atomic<uint64_t> score_dur_count_{0};
};

const char* ToString(RerankerMetrics::CoremlFallbackReason reason);
const char* ToString(RerankerMetrics::FailedTaskReason reason);

}  // namespace cortrix::reranker
