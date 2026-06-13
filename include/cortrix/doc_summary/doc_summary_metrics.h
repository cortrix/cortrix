#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace cortrix::doc_summary {

/// The F41 Document Summary metrics (F41 §12). Two OBS subsystems:
///   - `doc_summary`: llm_calls_total / llm_duration_seconds /
///     summaries_generated_total / fallback_triggered_total
///   - `fts5_fallback`: fts5_fallback_failed_total (F41-8 hybrid fallback)
/// Naming cortrix_<subsystem>_<metric>_<unit>. NO high-cardinality labels (§12
/// forbids tenant/ns/unit/user/request/doc id); doc_id etc. go to structured
/// logs only.
///
/// Standalone (D3): a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer (same pattern as F03 EnricherMetrics / F38 HypeMetrics / F40
/// SparseMetrics). The F24 `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder there is cross-Feature wiring → D3.5.
class DocSummaryMetrics {
public:
    /// status label for cortrix_doc_summary_llm_calls_total (§12 / §10.2).
    enum class LlmCallStatus { kStarted = 0, kSuccess, kFailed };

    /// result label for cortrix_doc_summary_fallback_triggered_total (F41-5 G).
    enum class FallbackResult { kHit = 0, kMiss };

    /// Process-wide instance (metrics are global counters/gauges).
    static DocSummaryMetrics& Instance();

    // --- cortrix_doc_summary_llm_calls_total (Counter, label: status) ---
    void RecordLlmCall(LlmCallStatus status);
    uint64_t LlmCallCount(LlmCallStatus status) const;

    // --- cortrix_doc_summary_llm_duration_seconds (Histogram, label: model) ---
    void ObserveLlmDuration(const std::string& model, double seconds);
    uint64_t LlmDurationCount(const std::string& model) const;
    double LlmDurationSum(const std::string& model) const;

    // --- cortrix_doc_summary_summaries_generated_total (Counter, no label) ---
    void AddSummariesGenerated(uint64_t n);
    uint64_t SummariesGeneratedCount() const;

    // --- cortrix_doc_summary_fallback_triggered_total (Counter, label: result) ---
    void RecordFallbackTriggered(FallbackResult result);
    uint64_t FallbackTriggeredCount(FallbackResult result) const;

    // --- cortrix_fts5_fallback_failed_total (Counter, no label) — subsystem fts5_fallback ---
    void RecordFts5FallbackFailed();
    uint64_t Fts5FallbackFailedCount() const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/histograms (test-only — production metrics are monotonic).
    void ResetForTest();

    // llm_duration_seconds histogram bucket count (upper bounds live in the .cpp
    // kDurBounds[]; +1 implicit +Inf). Public so the .cpp's bound arrays can size
    // against it; it is a benign implementation constant.
    static constexpr int kDurBucketCount = 7;  // 0.1,0.25,0.5,1,2,5,10

private:
    DocSummaryMetrics() = default;

    static constexpr int kLlmStatusCount = 3;
    static constexpr int kFallbackResultCount = 2;

    struct Histogram {
        std::array<uint64_t, kDurBucketCount + 1> buckets{};  // last = +Inf
        double sum = 0.0;
        uint64_t count = 0;
    };

    std::array<std::atomic<uint64_t>, kLlmStatusCount> llm_calls_{};
    std::array<std::atomic<uint64_t>, kFallbackResultCount> fallback_triggered_{};
    std::atomic<uint64_t> summaries_generated_{0};
    std::atomic<uint64_t> fts5_fallback_failed_{0};

    // Per-model histogram (model is low-cardinality, §12 label=model). Guarded by
    // mu_ because the map structure mutates (atomics can't cover map growth).
    mutable std::mutex mu_;
    std::map<std::string, Histogram> llm_duration_;
};

const char* ToString(DocSummaryMetrics::LlmCallStatus status);
const char* ToString(DocSummaryMetrics::FallbackResult result);

}  // namespace cortrix::doc_summary
