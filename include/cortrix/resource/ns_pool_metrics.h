#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <ostream>
#include <string>

namespace cortrix::resource {

/// The `ns_pool` subsystem metrics (observability naming,
/// naming `cortrix_ns_pool_<metric>_<unit>`). Self-contained dependency-free
/// recorder (same pattern as ScoringMetrics / Mem02Metrics): a process-wide
/// singleton of atomic gauges/counters/histograms + an OpenMetrics text renderer.
///
/// 🚨 Cardinality control: labels are
/// enum-only (`reason` for rejected_creates_total). NO namespace_id / unit_id /
/// tenant_id (high cardinality); per-NS data goes through the pool explain/stats
/// API (the namespace_id inside that JSON is a data field, not a metric label).
///
/// 🚨 D3 standalone: this recorder + RenderOpenMetrics() are fully usable +
/// testable in-process. The `/metrics` scrape endpoint does not exist in the
/// frozen tree — registering this recorder into that endpoint is cross-Feature
/// wiring **deferred to D3.5**. This makes explicit the registration convention that
/// namespace_pool.cpp:26-32 previously left only as a code comment: the A-class
/// values were already exposed via GetPoolStats()/StartupReport; this recorder is
/// the metric half, fed alongside the existing structured-log half.
///
/// §10.1 metric schema (6 rows):
///   cortrix_ns_pool_size                          gauge      (no label)
///   cortrix_ns_pool_memory_budget_used_bytes      gauge      (no label)
///   cortrix_ns_pool_rejected_creates_total        counter    {reason}
///   cortrix_ns_pool_startup_load_failures_total   counter    (no label)
///   cortrix_ns_pool_startup_load_duration_seconds histogram  (no label)
///   cortrix_ns_pool_ns_load_duration_seconds      histogram  (no label)
class NsPoolMetrics {
public:
    /// `reason` label for rejected_creates_total (§10.1 — the two admission gates).
    /// Values match the PoolStats.rejected_creates_total struct field names + the
    /// RejectionEvent.reason strings 1:1.
    enum class RejectReason {
        kNsCountExceeded = 0,
        kMemoryExceeded,
    };

    /// Process-wide instance (metrics are global gauges/counters/histograms).
    static NsPoolMetrics& Instance();

    // --- cortrix_ns_pool_size (Gauge, no label) ---
    // Current resident namespace count (pool_size_current).
    void SetSize(int64_t size);
    int64_t Size() const;

    // --- cortrix_ns_pool_memory_budget_used_bytes (Gauge, no label) ---
    void SetMemoryBudgetUsedBytes(int64_t bytes);
    int64_t MemoryBudgetUsedBytes() const;

    // --- cortrix_ns_pool_rejected_creates_total (Counter, label: reason) ---
    void RecordRejectedCreate(RejectReason reason);
    uint64_t RejectedCreateCount(RejectReason reason) const;

    // --- cortrix_ns_pool_startup_load_failures_total (Counter, no label) ---
    void AddStartupLoadFailures(uint64_t n);
    uint64_t StartupLoadFailuresCount() const;

    // --- cortrix_ns_pool_startup_load_duration_seconds (Histogram, no label) ---
    // The whole StartupLoadAll() wall-clock duration.
    void ObserveStartupLoadDuration(double seconds);

    // --- cortrix_ns_pool_ns_load_duration_seconds (Histogram, no label) ---
    // Per-namespace load latency (one observation per LoadOneNamespace).
    void ObserveNsLoadDuration(double seconds);

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all gauges/counters/histograms (test-only — production is monotonic).
    void ResetForTest();

    /// Number of explicit `le` buckets in the duration histograms (trailing +Inf
    /// implicit). Public so the .cpp bucket-bound tables (+ tests) can size against it.
    static constexpr int kNumDurBuckets = 8;  // {0.1,0.5,1,3,5,10,30,60}s

private:
    NsPoolMetrics() = default;

    static constexpr int kReasonCount = 2;  // ns_count_exceeded / memory_exceeded

    // A no-label duration histogram: non-cumulative buckets, last = +Inf, plus a
    // sum (integer microseconds) + count.
    struct Hist {
        std::array<std::atomic<uint64_t>, kNumDurBuckets + 1> bkt{};
        std::atomic<uint64_t> sum_us{0};
        std::atomic<uint64_t> count{0};
    };

    // Render one histogram as cumulative OpenMetrics _bucket{le=...}/_sum/_count
    // lines under `name` (no labels). Static so it can touch the private Hist.
    static void RenderHist(std::ostream& os, const char* name, const Hist& h);

    std::atomic<int64_t> size_{0};
    std::atomic<int64_t> memory_budget_used_bytes_{0};
    std::array<std::atomic<uint64_t>, kReasonCount> rejected_creates_{};
    std::atomic<uint64_t> startup_load_failures_{0};
    Hist startup_load_dur_{};
    Hist ns_load_dur_{};
};

const char* ToString(NsPoolMetrics::RejectReason reason);

}  // namespace cortrix::resource
