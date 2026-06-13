#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::memory::transparency {

/// The `memory_transparency` subsystem metrics (MEM03 §8, OBSERVABILITY_SPEC §2
/// naming `cortrix_memory_transparency_<metric>_<unit>`, §2.3 line 105 SoT). Mirrors
/// the mem02_metrics.h / F36 RagFusionMetrics template (process-wide singleton,
/// atomic counters/histograms, OpenMetrics renderer).
///
/// 🚨 Cardinality control (OBSERVABILITY_SPEC §3.2 — MEM03 §8): labels are enum-only.
/// NO `tenant_id` / `ns_id` / `user_id` (high-cardinality, forbidden); per-NS /
/// per-tenant data goes through the §3.4 per-tenant API. `op` is the 4-value op enum
/// (list/create/edit/invalidate — V8 G2 M1: op=invalidate is 1:1 with the audit
/// action name), `status` is success/error, `error_code` is the 5-value MEM03
/// error-code enum (all bounded, low-cardinality).
///
/// 🚨 D3 standalone: a self-contained, dependency-free recorder + an OpenMetrics text
/// renderer. The F24 `/metrics` scrape endpoint does not exist in the frozen tree —
/// registering this recorder into that endpoint is cross-Feature wiring **deferred to
/// D3.5**. Until then it is fully usable + testable in-process and RenderOpenMetrics()
/// produces what F24 will serve.
///
/// §8 metric schema (5 rows):
///   cortrix_memory_transparency_op_total                counter   {op, status}
///   cortrix_memory_transparency_op_latency_seconds      histogram {op}
///   cortrix_memory_transparency_cross_user_blocked_total counter  {}  (L1.bis 404 mask)
///   cortrix_memory_transparency_edit_conflict_total     counter   {}  (optimistic-lock conflict)
///   cortrix_memory_transparency_invalid_input_total     counter   {error_code}
class Mem03Metrics {
public:
    /// op label for op_total / op_latency_seconds (§8). list/create/edit/invalidate —
    /// `invalidate` (not `delete`) is 1:1 with the audit action + P12 tool name
    /// (V8 G2 M1 naming sync, MEM03 §8 / §4.2).
    enum class Op {
        kList = 0,
        kCreate,
        kEdit,
        kInvalidate,
    };

    /// status label for op_total (§8).
    enum class OpStatus {
        kSuccess = 0,
        kError,
    };

    /// error_code label for invalid_input_total (§8). The 5 registered MEM03 error
    /// identities (mem03_error.h Mem03ErrorCode), kept enum-bounded so the metric
    /// label stays low-cardinality. Ordered to match Mem03ErrorCode.
    enum class ErrorCodeLabel {
        kMemoryNotFound = 0,
        kUserMismatch,
        kAlreadyInvalidated,
        kInvalidateFailed,
        kQuota,
    };

    /// Process-wide instance (metrics are global counters/histograms).
    static Mem03Metrics& Instance();

    // --- cortrix_memory_transparency_op_total (Counter, labels: op, status) ---
    void RecordOp(Op op, OpStatus status);
    uint64_t OpCount(Op op, OpStatus status) const;

    // --- cortrix_memory_transparency_op_latency_seconds (Histogram, label: op) ---
    // Observes the end-to-end latency for `op` (a low-cardinality enum). Tracks
    // per-op sum_ms + count + cumulative buckets (rendered seconds).
    void ObserveOpLatency(Op op, int latency_ms);

    // --- cortrix_memory_transparency_cross_user_blocked_total (Counter) ---
    // L1.bis 404-mask trigger count (a cross-user access was masked as not-found).
    void RecordCrossUserBlocked();
    uint64_t CrossUserBlockedCount() const;

    // --- cortrix_memory_transparency_edit_conflict_total (Counter) ---
    // Optimistic-lock (expected_modified_at mismatch) conflict count.
    void RecordEditConflict();
    uint64_t EditConflictCount() const;

    // --- cortrix_memory_transparency_invalid_input_total (Counter, label: error_code) ---
    void RecordInvalidInput(ErrorCodeLabel error_code);
    uint64_t InvalidInputCount(ErrorCodeLabel error_code) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/histograms (test-only — production metrics are monotonic).
    void ResetForTest();

    /// Number of explicit `le` buckets in the op_latency_seconds histogram (a
    /// trailing +Inf bucket is implicit). Public so the .cpp bucket-bound tables (and
    /// tests) can size against it; the bound values live in the .cpp.
    static constexpr int kNumDurBuckets = 8;  // {0.005,0.01,0.025,0.05,0.1,0.25,0.5,1}

private:
    Mem03Metrics() = default;

    static constexpr int kOpCount = 4;          // list / create / edit / invalidate
    static constexpr int kOpStatusCount = 2;    // success / error
    static constexpr int kErrorCodeCount = 5;   // the 5 Mem03ErrorCode identities

    // op_total[op][status]
    std::array<std::array<std::atomic<uint64_t>, kOpStatusCount>, kOpCount> op_total_{};
    // op_latency: per-op sum_ms + count + cumulative-histogram buckets (last = +Inf).
    std::array<std::atomic<uint64_t>, kOpCount> latency_sum_ms_{};
    std::array<std::atomic<uint64_t>, kOpCount> latency_count_{};
    std::array<std::array<std::atomic<uint64_t>, kNumDurBuckets + 1>, kOpCount> latency_bkt_{};
    std::atomic<uint64_t> cross_user_blocked_{0};
    std::atomic<uint64_t> edit_conflict_{0};
    std::array<std::atomic<uint64_t>, kErrorCodeCount> invalid_input_{};
};

const char* ToString(Mem03Metrics::Op op);
const char* ToString(Mem03Metrics::OpStatus status);
const char* ToString(Mem03Metrics::ErrorCodeLabel error_code);

}  // namespace cortrix::memory::transparency
