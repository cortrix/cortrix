#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "cortrix/async/task_type.h"

namespace cortrix::async {

/// The async-task subsystem metrics (observability naming). The metric names
/// are `cortrix_tasks_<metric>_<unit>` — the locked observability names, which
/// never embed a tracking label. Self-contained
/// dependency-free recorder (same pattern as ScoringMetrics / MemoryExtractMetrics): a
/// process-wide singleton of atomic counters/gauges/histograms + an OpenMetrics
/// text renderer.
///
/// 🚨 Cardinality control (the observability spec): labels are enum-only —
/// `task_type` (the closed async::TaskType enum), `status`, `state`, `phase`. No
/// task_id / namespace_id / doc_id (high cardinality); per-task data goes through
/// the tasks table + progress API, never a metric label.
///
/// 🚨 standalone: this recorder + RenderOpenMetrics() are fully usable +
/// testable in-process. The `/metrics` scrape endpoint does not exist in the
/// frozen tree — registering this recorder into that endpoint is cross-Feature
/// wiring **deferred to integration** (B-R2-FREEZE-REVIEW lists "OBSERVABILITY
/// async metrics" as deferred wiring; the recorder itself is the piece).
///
/// metric schema (6 rows):
///   cortrix_tasks_submitted_total       counter    {task_type}
///   cortrix_tasks_completed_total       counter    {task_type, status}
///   cortrix_tasks_duration_seconds      histogram  {task_type}
///   cortrix_tasks_queue_depth           gauge      {state}
///   cortrix_tasks_zombie_cleaned_total  counter    (no label)
///   cortrix_tasks_cancel_total          counter    {phase}
class TaskMetrics {
public:
    /// `status` label for completed_total. The terminal outcome of a task.
    enum class CompletionStatus {
        kSuccess = 0,
        kFailed,
        kCancelled,
        kTimeout,   ///< the 30-min budget overrun (CX_ERR_TASK_TIMEOUT)
    };

    /// `state` label for queue_depth. The two countable queue states.
    enum class QueueState {
        kQueued = 0,
        kProcessing,
    };

    /// `phase` label for cancel_total. When in the task lifecycle the
    /// DELETE /tasks/{id} cancel landed.
    enum class CancelPhase {
        kPreDequeue = 0,    ///< queued task cancelled before a Worker picked it up
        kMidProcessing,     ///< signalled while processing (status → cancelling)
        kPostChunkIdx,      ///< checkpoint hit after the chunk-index point
    };

    /// Process-wide instance (metrics are global counters/gauges/histograms).
    static TaskMetrics& Instance();

    // --- cortrix_tasks_submitted_total (Counter, label: task_type) ---
    void RecordSubmitted(TaskType task_type);
    uint64_t SubmittedCount(TaskType task_type) const;

    // --- cortrix_tasks_completed_total (Counter, labels: task_type, status) ---
    void RecordCompleted(TaskType task_type, CompletionStatus status);
    uint64_t CompletedCount(TaskType task_type, CompletionStatus status) const;

    // --- cortrix_tasks_duration_seconds (Histogram, label: task_type) ---
    // End-to-end task execution latency (dequeue + Worker processing + persistence).
    void ObserveDuration(TaskType task_type, double seconds);

    // --- cortrix_tasks_queue_depth (Gauge, label: state) ---
    void SetQueueDepth(QueueState state, int64_t depth);
    int64_t QueueDepth(QueueState state) const;

    // --- cortrix_tasks_zombie_cleaned_total (Counter, no label) ---
    // Increment by the number of zombie tasks the cron swept to failed.
    void AddZombieCleaned(uint64_t n);
    uint64_t ZombieCleanedCount() const;

    // --- cortrix_tasks_cancel_total (Counter, label: phase) ---
    void RecordCancel(CancelPhase phase);
    uint64_t CancelCount(CancelPhase phase) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauges/histograms (test-only — production is monotonic).
    void ResetForTest();

    /// Number of explicit `le` buckets in duration_seconds (trailing +Inf implicit).
    /// Public so the .cpp bucket-bound tables (+ tests) can size against it.
    static constexpr int kNumDurBuckets = 8;  // {1,5,15,30,60,300,900,1800}s

private:
    TaskMetrics() = default;

    // The async::TaskType enum is closed (1/2/3); index it 0-based via TaskTypeIndex.
    static constexpr int kTaskTypeCount = 3;
    static constexpr int kStatusCount = 4;   // success / failed / cancelled / timeout
    static constexpr int kStateCount = 2;    // queued / processing
    static constexpr int kPhaseCount = 3;    // pre_dequeue / mid_processing / post_chunk_idx

    // Per-task_type duration histogram: non-cumulative buckets [type][bucket],
    // last bucket = +Inf, plus sum (stored as integer microseconds) + count.
    struct Hist {
        std::array<std::atomic<uint64_t>, kNumDurBuckets + 1> bkt{};
        std::atomic<uint64_t> sum_us{0};
        std::atomic<uint64_t> count{0};
    };

    std::array<std::atomic<uint64_t>, kTaskTypeCount> submitted_{};
    std::array<std::array<std::atomic<uint64_t>, kStatusCount>, kTaskTypeCount>
        completed_{};
    std::array<Hist, kTaskTypeCount> duration_{};
    std::array<std::atomic<int64_t>, kStateCount> queue_depth_{};
    std::atomic<uint64_t> zombie_cleaned_{0};
    std::array<std::atomic<uint64_t>, kPhaseCount> cancel_{};
};

/// Map an async::TaskType enum value (1/2/3) to a 0-based table index. An unknown
/// value folds into the doc-parse slot (0) so the recorder stays bounded.
int TaskTypeIndex(TaskType task_type);

const char* ToString(TaskType task_type);
const char* ToString(TaskMetrics::CompletionStatus status);
const char* ToString(TaskMetrics::QueueState state);
const char* ToString(TaskMetrics::CancelPhase phase);

}  // namespace cortrix::async
