#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace cortrix::observability {

/// The operation_log metrics (6 metrics, observability
/// naming `cortrix_<metric>_<unit>`). Mirrors the AgentTraceMetrics template
/// (process-wide singleton, atomic counters/gauge/histogram + OpenMetrics renderer).
///
/// Cardinality control (OBSERVABILITY_SPEC §3.2 — C5 decision): labels are
/// low-cardinality only. NO user_id / namespace_id / trace_id labels (forbidden,
/// high-cardinality). `action` is the §5.1 {resource}_{verb} string (≤20 distinct
/// in practice); `resource_type` is one of the §9.1 site categories. Because
/// `action` is a free string (not a fixed enum), writes_total is held in a
/// mutex-guarded map keyed by {action, resource_type} rather than a fixed array.
///
/// §11 metric schema (6 rows):
///   cortrix_oplog_writes_total              counter   {action, resource_type}
///   cortrix_oplog_query_latency_seconds     histogram {filter_dimensions} (0-8)
///   cortrix_oplog_cleanup_deleted_total     counter   {reason} (age|quota)
///   cortrix_oplog_cleanup_duration_seconds  histogram (no label)
///   cortrix_oplog_cleanup_failed_total      counter   (no label)
///   cortrix_oplog_size_rows                 gauge     (no label)
///
/// 🚨 Strictly additive to the business path (C4): every Record* call is a no-throw,
/// lock-light counter bump. The :9091 /metrics endpoint registers this recorder's
/// RenderOpenMetrics() the same way as every other subsystem (bootstrap.cpp).
class OplogMetrics {
public:
    /// reason label for cleanup_deleted_total (§8.3 — age window vs row-cap quota).
    enum class CleanupReason { kAge = 0, kQuota };

    /// Process-wide instance (metrics are global counters/gauge/histogram).
    static OplogMetrics& Instance();

    // --- cortrix_oplog_writes_total (Counter, labels: action, resource_type) ---
    /// `action` is bounded-by-convention: it is the closed action
    /// vocabulary (CE domain = 20 values, `{resource}_{verb}` naming), NOT free user
    /// input — so it is low-cardinality and safe as a label (OBSERVABILITY_SPEC §3.2).
    /// `resource_type` is one of the 6 §9.1 site categories. A caller passing an
    /// off-vocabulary action would be a caller bug, not a design defect (mirrors the
    /// agent_trace bounded-label posture).
    void RecordWrite(const std::string& action, const std::string& resource_type);
    uint64_t WriteCount(const std::string& action,
                        const std::string& resource_type) const;

    // --- cortrix_oplog_query_latency_seconds (Histogram, label: filter_dimensions) ---
    /// @param filter_dimensions number of active query filter fields (0-8, clamped).
    void ObserveQueryLatency(int filter_dimensions, int latency_ms);
    uint64_t QueryLatencyCount(int filter_dimensions) const;

    // --- cortrix_oplog_cleanup_deleted_total (Counter, label: reason) ---
    void RecordCleanupDeleted(CleanupReason reason, int64_t count);
    uint64_t CleanupDeletedCount(CleanupReason reason) const;

    // --- cortrix_oplog_cleanup_duration_seconds (Histogram, no label) ---
    void ObserveCleanupDuration(int duration_ms);
    uint64_t CleanupDurationCount() const;

    // --- cortrix_oplog_cleanup_failed_total (Counter, no label) ---
    void RecordCleanupFailed();
    uint64_t CleanupFailedCount() const;

    // --- cortrix_oplog_size_rows (Gauge, no label) ---
    void SetSizeRows(int64_t rows);
    int64_t SizeRows() const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauge/histogram (test-only — production metrics are monotonic).
    void ResetForTest();

    /// Number of explicit `le` buckets in the cleanup_duration_seconds histogram
    /// (a trailing +Inf bucket is implicit). Cleanup sweeps a SQLite table, so the
    /// bounds run wider than the sub-second query histogram.
    static constexpr int kNumCleanupBuckets = 8;  // {0.01,0.05,0.1,0.5,1,5,10,30}

    /// filter_dimensions histogram: one series per dimension count 0..8 (§11 label
    /// "filter_dimensions (0-8 values)"); index 8 also absorbs any clamp overflow.
    static constexpr int kMaxFilterDimensions = 8;
    static constexpr int kFilterDimensionSeries = kMaxFilterDimensions + 1;  // 0..8

    /// Number of explicit `le` buckets in the query_latency_seconds histogram.
    static constexpr int kNumQueryBuckets = 8;  // {0.005,0.01,0.025,0.05,0.1,0.25,0.5,1}

private:
    OplogMetrics() = default;

    static constexpr int kCleanupReasonCount = 2;  // age / quota

    // writes_total[{action, resource_type}] — free-string keys → map under a mutex.
    mutable std::mutex writes_mu_;
    std::map<std::pair<std::string, std::string>, uint64_t> writes_;

    // cleanup_deleted_total[reason]
    std::array<std::atomic<uint64_t>, kCleanupReasonCount> cleanup_deleted_{};
    std::atomic<uint64_t> cleanup_failed_{0};
    std::atomic<int64_t> size_rows_{0};

    // query_latency_seconds histogram — one (sum,count,buckets) triple per
    // filter_dimensions value 0..8.
    struct Histo {
        std::atomic<uint64_t> sum_ms{0};
        std::atomic<uint64_t> count{0};
        std::array<std::atomic<uint64_t>, kNumQueryBuckets + 1> bkt{};  // last = +Inf
    };
    std::array<Histo, kFilterDimensionSeries> q_lat_{};

    // cleanup_duration_seconds histogram (single series, no label).
    std::atomic<uint64_t> cln_sum_ms_{0};
    std::atomic<uint64_t> cln_count_{0};
    std::array<std::atomic<uint64_t>, kNumCleanupBuckets + 1> cln_bkt_{};  // last = +Inf
};

const char* ToString(OplogMetrics::CleanupReason reason);

}  // namespace cortrix::observability
