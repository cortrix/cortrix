#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace cortrix::spc {

/// The Contextual Retrieval subsystem metrics (observability
/// subsystem `contextual_retrieval`). Naming
/// cortrix_contextual_retrieval_<metric>_<unit>. NO high-cardinality labels
/// (V3 ruling 10 dropped the ns_id label; per-NS data is served via the
/// system/namespaces/<id>/stats endpoint — OBS_SPEC forbids
/// tenant/ns/unit/user/request/chunk id); chunk_id etc. go to structured logs only.
///
/// Standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer (same pattern as EnricherMetrics / HypeMetrics /
/// SparseMetrics / OnnxMetrics). The `/metrics` scrape endpoint does not
/// exist in the frozen tree — registering this recorder there is cross-Feature
/// wiring → integration.
class ContextualRetrievalMetrics {
public:
    /// status label for cortrix_contextual_retrieval_chunks_total — the
    /// write-time outcome per chunk. Mirrors contextualized_status 1/2/3.
    enum class ChunkStatus { kGenerated = 0, kFailed, kSkippedNoLlm };

    /// path label for cortrix_contextual_retrieval_query_via_path_total —
    /// which recall path served a query hit (B-class explain via_path).
    enum class QueryPath { kDense = 0, kContextualized, kFallbackDense };

    /// Process-wide instance (metrics are global counters/gauges).
    static ContextualRetrievalMetrics& Instance();

    // --- cortrix_contextual_retrieval_chunks_total (Counter, label: status) ---
    void RecordChunk(ChunkStatus status);
    uint64_t ChunkCount(ChunkStatus status) const;

    // --- cortrix_contextual_retrieval_llm_cost_tokens (Counter, label: model) ---
    void AddLlmCostTokens(const std::string& model, int64_t tokens);
    int64_t LlmCostTokens(const std::string& model) const;

    // --- cortrix_contextual_retrieval_fallback_ratio (Gauge, no label) ---
    // The L3 fallback ratio in [0,1]. Stored as a scaled integer (x1e6) so the
    // recorder stays lock-free; rendered back to a float ratio.
    void SetFallbackRatio(double ratio);
    double FallbackRatio() const;

    // --- cortrix_contextual_retrieval_query_via_path_total (Counter, label: path) ---
    void RecordQueryPath(QueryPath path);
    uint64_t QueryPathCount(QueryPath path) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauges (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    ContextualRetrievalMetrics() = default;

    static constexpr int kChunkStatusCount = 3;
    static constexpr int kQueryPathCount = 3;
    static constexpr double kRatioScale = 1e6;

    std::array<std::atomic<uint64_t>, kChunkStatusCount> chunks_{};
    std::array<std::atomic<uint64_t>, kQueryPathCount> query_paths_{};
    std::atomic<int64_t> fallback_ratio_scaled_{0};

    // Per-model token counter (model is a bounded-cardinality label). Guarded by
    // mu_ because the map structure mutates (atomics can't cover map growth).
    mutable std::mutex mu_;
    std::map<std::string, int64_t> cost_tokens_by_model_;
};

const char* ToString(ContextualRetrievalMetrics::ChunkStatus status);
const char* ToString(ContextualRetrievalMetrics::QueryPath path);

}  // namespace cortrix::spc
