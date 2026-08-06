#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace cortrix::memory {

/// The `mem02` subsystem metrics (observability naming
/// `cortrix_memory_extract_<metric>_<unit>`). Mirrors the RagFusionMetrics /
/// ScatterMetrics template (process-wide singleton, atomic counters/histograms,
/// OpenMetrics renderer).
///
/// 🚨 Cardinality control: labels are
/// enum-only or a low-cardinality config-driven model string. NO `tenant_id` /
/// `ns_id` / `user_id` (high-cardinality, forbidden); per-NS / per-tenant data
/// goes through the §3.4 per-tenant API.
///
/// 🚨 D3 standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer. The `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder into that endpoint is cross-Feature wiring
/// **deferred to D3.5** (memory-extraction-rev-7). Until then it is fully usable + testable
/// in-process and RenderOpenMetrics() produces what the server will serve.
///
/// §5.4.1 metric schema (6 rows):
///   cortrix_memory_extract_total                counter   {status: success|failed}
///   cortrix_memory_extract_duration_seconds     histogram {model}
///   cortrix_memory_extract_queue_depth                  gauge     {worker_id}  (process-aggregate)
///   cortrix_memory_extract_llm_tokens_total             counter   {model, direction: input|output}
///   cortrix_memory_extract_contradictions_found_total   counter   {confidence_bucket: high|medium|low}
///   cortrix_memory_extract_invalidations_total          counter   {triggered_by: llm_auto|manual|agent_self}
class MemoryExtractMetrics {
public:
    /// status label for extract_total (§5.4.1).
    enum class ExtractStatus {
        kSuccess = 0,
        kFailed,
    };

    /// direction label for llm_tokens_total (§5.4.1).
    enum class TokenDirection {
        kInput = 0,
        kOutput,
    };

    /// confidence_bucket label for contradictions_found_total (§5.4.1).
    /// high = [0.8,1.0], medium = [0.5,0.8), low = [0,0.5).
    enum class ConfidenceBucket {
        kHigh = 0,
        kMedium,
        kLow,
    };

    /// triggered_by label for invalidations_total (§5.4.1). `agent_self` is the
    /// ASCII rendering of the spec's "agent self-service" (label values stay ASCII for
    /// Prometheus compatibility).
    enum class TriggeredBy {
        kLlmAuto = 0,
        kManual,
        kAgentSelf,
    };

    /// Map a raw confidence in [0,1] to its bucket label.
    static ConfidenceBucket BucketForConfidence(double confidence);

    /// Process-wide instance (metrics are global counters/histograms).
    static MemoryExtractMetrics& Instance();

    // --- cortrix_memory_extract_total (Counter, label: status) ---
    void RecordExtract(ExtractStatus status);
    uint64_t ExtractCount(ExtractStatus status) const;

    // --- cortrix_memory_extract_duration_seconds (Histogram, label: model) ---
    // Observes the end-to-end extraction latency for `model` (a low-cardinality
    // config-driven enum string). Tracks per-model sum_ms + count (rendered seconds).
    void ObserveExtractDuration(const std::string& model, int latency_ms);

    // --- cortrix_memory_extract_queue_depth (Gauge) ---
    // Process-aggregate queue depth (the per-worker_id label is reserved; V1
    // reports the single shared queue's depth). Set to the current size on push/pop.
    void SetQueueDepth(int64_t depth);
    int64_t QueueDepth() const;

    // --- cortrix_memory_extract_llm_tokens_total (Counter, labels: model, direction) ---
    void RecordTokens(const std::string& model, TokenDirection direction, uint64_t tokens);
    uint64_t TokenCount(TokenDirection direction) const;  // summed over models

    // --- cortrix_memory_extract_contradictions_found_total (Counter, label: confidence_bucket) ---
    void RecordContradiction(ConfidenceBucket bucket);
    uint64_t ContradictionCount(ConfidenceBucket bucket) const;

    // --- cortrix_memory_extract_invalidations_total (Counter, label: triggered_by) ---
    void RecordInvalidation(TriggeredBy triggered_by);
    uint64_t InvalidationCount(TriggeredBy triggered_by) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/histograms (test-only — production metrics are monotonic).
    void ResetForTest();

    /// Number of explicit `le` buckets in the extract_duration_seconds histogram
    /// (a trailing +Inf bucket is implicit). Public so the .cpp bucket-bound tables
    /// (and tests) can size against it; the bound values live in the .cpp.
    static constexpr int kNumDurBuckets = 8;  // {0.1,0.25,0.5,1,2,5,10,30}

private:
    MemoryExtractMetrics() = default;

    static constexpr int kExtractStatusCount = 2;     // success / failed
    static constexpr int kDirectionCount = 2;         // input / output
    static constexpr int kConfidenceBucketCount = 3;  // high / medium / low
    static constexpr int kTriggeredByCount = 3;       // llm_auto / manual / agent_self
    // Per-model latency/token table: a tiny fixed table keyed by model name (low
    // cardinality). Models beyond the table fold into a shared "other" slot so the
    // recorder stays allocation-free + bounded on the hot path.
    static constexpr int kModelSlots = 8;

    /// Resolve (or assign) the table slot for `model`; the last slot is the shared
    /// overflow bucket. Caller holds model_mu_.
    int ModelSlotLocked(const std::string& model);

    std::array<std::atomic<uint64_t>, kExtractStatusCount> extract_{};
    std::array<std::atomic<uint64_t>, kDirectionCount> token_{};
    std::array<std::atomic<uint64_t>, kConfidenceBucketCount> contradiction_{};
    std::array<std::atomic<uint64_t>, kTriggeredByCount> invalidation_{};
    std::atomic<int64_t> queue_depth_{0};

    // model tables (guarded by model_mu_; the counters above are lock-free, but the
    // model string→slot mapping needs a small critical section).
    mutable std::mutex model_mu_;
    std::array<std::string, kModelSlots> model_names_{};
    std::array<std::atomic<uint64_t>, kModelSlots> model_latency_sum_ms_{};
    std::array<std::atomic<uint64_t>, kModelSlots> model_latency_count_{};
    // Per-model cumulative-histogram buckets: [slot][bucket], last bucket = +Inf.
    std::array<std::array<std::atomic<uint64_t>, kNumDurBuckets + 1>, kModelSlots>
        model_latency_bkt_{};
    std::array<std::atomic<uint64_t>, kModelSlots> model_token_in_{};
    std::array<std::atomic<uint64_t>, kModelSlots> model_token_out_{};
};

const char* ToString(MemoryExtractMetrics::ExtractStatus status);
const char* ToString(MemoryExtractMetrics::TokenDirection direction);
const char* ToString(MemoryExtractMetrics::ConfidenceBucket bucket);
const char* ToString(MemoryExtractMetrics::TriggeredBy triggered_by);

}  // namespace cortrix::memory
