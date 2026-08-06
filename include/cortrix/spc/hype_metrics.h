#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace cortrix::spc {

/// The HyPE-index subsystem metrics (observability subsystem
/// `hype_index`). Naming cortrix_hype_index_<metric>_<unit>. NO high-cardinality
/// labels (/ OBS_SPEC forbids tenant/ns/unit/user/request/chunk id);
/// chunk_id etc. go to structured logs only (HypeLog, below).
///
/// Standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer (same pattern as EnricherMetrics / SparseMetrics /
/// OnnxMetrics). The `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder there is cross-Feature wiring → integration.
class HypeMetrics {
public:
    /// status label for cortrix_hype_index_llm_calls_total.
    enum class LlmCallStatus { kSuccess = 0, kFailed };

    /// hit_type label for cortrix_hype_index_match_total.
    enum class MatchHitType { kChunk = 0, kHype, kBoth };

    /// Process-wide instance (metrics are global counters/gauges).
    static HypeMetrics& Instance();

    // --- cortrix_hype_index_llm_calls_total (Counter, label: status) ---
    void RecordLlmCall(LlmCallStatus status);
    uint64_t LlmCallCount(LlmCallStatus status) const;

    // --- cortrix_hype_index_llm_duration_seconds (Histogram, label: model) ---
    // Observes the LLM call latency under the given model label (low cardinality).
    void ObserveLlmDuration(const std::string& model, double seconds);
    uint64_t LlmDurationCount(const std::string& model) const;
    double LlmDurationSum(const std::string& model) const;

    // --- cortrix_hype_index_questions_generated_total (Counter, no label) ---
    void AddQuestionsGenerated(uint64_t n);
    uint64_t QuestionsGeneratedCount() const;

    // --- cortrix_hype_index_match_total (Counter, label: hit_type) ---
    void RecordMatch(MatchHitType hit_type);
    uint64_t MatchCount(MatchHitType hit_type) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/histograms (test-only — production metrics are monotonic).
    void ResetForTest();

    // llm_duration_seconds histogram bucket count (upper bounds live in the .cpp
    // kDurBounds[]; +1 implicit +Inf). Public so the .cpp's bound arrays can size
    // against it; it is a benign implementation constant.
    static constexpr int kDurBucketCount = 7;  // 0.1,0.25,0.5,1,2,5,10

private:
    HypeMetrics() = default;

    static constexpr int kLlmStatusCount = 2;
    static constexpr int kMatchHitTypeCount = 3;

    struct Histogram {
        std::array<uint64_t, kDurBucketCount + 1> buckets{};  // last = +Inf
        double sum = 0.0;
        uint64_t count = 0;
    };

    std::array<std::atomic<uint64_t>, kLlmStatusCount> llm_calls_{};
    std::array<std::atomic<uint64_t>, kMatchHitTypeCount> matches_{};
    std::atomic<uint64_t> questions_generated_{0};

    // Per-model histogram (model is low-cardinality, label=model). Guarded by
    // mu_ because the map structure mutates (atomics can't cover map growth).
    mutable std::mutex mu_;
    std::map<std::string, Histogram> llm_duration_;
};

const char* ToString(HypeMetrics::LlmCallStatus status);
const char* ToString(HypeMetrics::MatchHitType hit_type);

}  // namespace cortrix::spc
