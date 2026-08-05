#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::retrieval {

/// The BGE-M3-sparse subsystem metrics (observability subsystem
/// `bge_m3_sparse`). Naming `cortrix_bge_m3_sparse_<metric>` (no ns_id label —
/// D1 V3 ruling 10 removed the high-cardinality label; per-NS data goes through
/// the namespaces/<ns_id>/stats API).
///
/// Standalone (D3): a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer (same pattern as RerankerMetrics / OnnxMetrics). The
/// `/metrics` scrape endpoint does not exist in the frozen tree — registering
/// this recorder into that endpoint is cross-Feature wiring → D3.5. Until then it
/// is fully usable + testable in-process and RenderOpenMetrics() produces what
/// the server will serve.
class SparseMetrics {
public:
    /// status label for cortrix_bge_m3_sparse_inference_total (§10).
    enum class InferenceStatus {
        kSuccess = 0,
        kFailed,
        kFallback,
    };

    /// path label for cortrix_bge_m3_sparse_query_via_path_total (§10).
    enum class ViaPath {
        kSparse = 0,             ///< query used the sparse path
        kFallbackDenseFts5,      ///< L2 fallback: dense + FTS5 (+hype)
    };

    /// Process-wide instance (metrics are global counters/gauges).
    static SparseMetrics& Instance();

    // --- cortrix_bge_m3_sparse_inference_total (Counter, label: status) ---
    void RecordInference(InferenceStatus status);
    uint64_t InferenceCount(InferenceStatus status) const;

    // --- cortrix_bge_m3_sparse_fallback_ratio (Gauge, no label) ---
    // Query fallback ratio (caller sets it from its own success/fallback tally).
    void SetFallbackRatio(double ratio);
    double FallbackRatio() const;

    // --- cortrix_bge_m3_sparse_inverted_index_size_bytes (Gauge, no label) ---
    void SetInvertedIndexSizeBytes(uint64_t bytes);
    uint64_t InvertedIndexSizeBytes() const;

    // --- cortrix_bge_m3_sparse_query_via_path_total (Counter, label: path) ---
    void RecordQueryViaPath(ViaPath path);
    uint64_t QueryViaPathCount(ViaPath path) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauges (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    SparseMetrics() = default;

    static constexpr int kInferenceStatusCount = 3;
    static constexpr int kViaPathCount = 2;

    std::array<std::atomic<uint64_t>, kInferenceStatusCount> inference_{};
    std::array<std::atomic<uint64_t>, kViaPathCount> via_path_{};
    // fallback_ratio gauge stored as double-bits for atomic last-write-wins.
    std::atomic<uint64_t> fallback_ratio_bits_{0};
    std::atomic<uint64_t> inverted_index_size_bytes_{0};
};

const char* ToString(SparseMetrics::InferenceStatus status);
const char* ToString(SparseMetrics::ViaPath path);

}  // namespace cortrix::retrieval
