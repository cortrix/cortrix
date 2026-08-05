#include <cstdint>
#include "cortrix/async/task_metrics.h"

#include <sstream>

namespace cortrix::async {

namespace {

// duration_seconds histogram bucket upper bounds (seconds). Small
// docs return sync ≤ 5 min (300s); large docs async ≤ 30 min; task_timeout_seconds
// = 1800 (30 min). Bounds straddle the 5-min sync threshold (300) and the 30-min
// timeout (1800) so the latency distribution + the timeout tail are both visible.
constexpr double kDurBounds[TaskMetrics::kNumDurBuckets] =
    {1.0, 5.0, 15.0, 30.0, 60.0, 300.0, 900.0, 1800.0};
constexpr const char* kDurBoundStr[TaskMetrics::kNumDurBuckets] =
    {"1", "5", "15", "30", "60", "300", "900", "1800"};

// Index of the bucket a `seconds` observation falls into (kNumDurBuckets = +Inf).
int DurBucketIndex(double seconds) {
    for (int j = 0; j < TaskMetrics::kNumDurBuckets; ++j) {
        if (seconds <= kDurBounds[j]) return j;
    }
    return TaskMetrics::kNumDurBuckets;  // +Inf
}

}  // namespace

int TaskTypeIndex(TaskType task_type) {
    switch (task_type) {
        case kTaskDocParse:      return 0;
        case kTaskWatcherFanout: return 1;
        case kTaskDocSummary:    return 2;
    }
    return 0;  // unknown folds into doc-parse slot (bounded)
}

TaskMetrics& TaskMetrics::Instance() {
    static TaskMetrics instance;
    return instance;
}

void TaskMetrics::RecordSubmitted(TaskType task_type) {
    submitted_[TaskTypeIndex(task_type)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t TaskMetrics::SubmittedCount(TaskType task_type) const {
    return submitted_[TaskTypeIndex(task_type)].load(std::memory_order_relaxed);
}

void TaskMetrics::RecordCompleted(TaskType task_type, CompletionStatus status) {
    completed_[TaskTypeIndex(task_type)][static_cast<int>(status)].fetch_add(
        1, std::memory_order_relaxed);
}

uint64_t TaskMetrics::CompletedCount(TaskType task_type, CompletionStatus status) const {
    return completed_[TaskTypeIndex(task_type)][static_cast<int>(status)].load(
        std::memory_order_relaxed);
}

void TaskMetrics::ObserveDuration(TaskType task_type, double seconds) {
    if (seconds < 0) seconds = 0;
    Hist& h = duration_[TaskTypeIndex(task_type)];
    h.bkt[DurBucketIndex(seconds)].fetch_add(1, std::memory_order_relaxed);
    h.count.fetch_add(1, std::memory_order_relaxed);
    h.sum_us.fetch_add(static_cast<uint64_t>(seconds * 1e6), std::memory_order_relaxed);
}

void TaskMetrics::SetQueueDepth(QueueState state, int64_t depth) {
    if (depth < 0) depth = 0;
    queue_depth_[static_cast<int>(state)].store(depth, std::memory_order_relaxed);
}

int64_t TaskMetrics::QueueDepth(QueueState state) const {
    return queue_depth_[static_cast<int>(state)].load(std::memory_order_relaxed);
}

void TaskMetrics::AddZombieCleaned(uint64_t n) {
    zombie_cleaned_.fetch_add(n, std::memory_order_relaxed);
}

uint64_t TaskMetrics::ZombieCleanedCount() const {
    return zombie_cleaned_.load(std::memory_order_relaxed);
}

void TaskMetrics::RecordCancel(CancelPhase phase) {
    cancel_[static_cast<int>(phase)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t TaskMetrics::CancelCount(CancelPhase phase) const {
    return cancel_[static_cast<int>(phase)].load(std::memory_order_relaxed);
}

std::string TaskMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_tasks_submitted_total (counter, label: task_type)
    os << "# HELP cortrix_tasks_submitted_total Async tasks submitted by task_type.\n";
    os << "# TYPE cortrix_tasks_submitted_total counter\n";
    for (TaskType tt : {kTaskDocParse, kTaskWatcherFanout, kTaskDocSummary}) {
        os << "cortrix_tasks_submitted_total{task_type=\"" << ToString(tt) << "\"} "
           << SubmittedCount(tt) << "\n";
    }

    // cortrix_tasks_completed_total (counter, labels: task_type, status)
    os << "# HELP cortrix_tasks_completed_total Async tasks completed by task_type and status.\n";
    os << "# TYPE cortrix_tasks_completed_total counter\n";
    for (TaskType tt : {kTaskDocParse, kTaskWatcherFanout, kTaskDocSummary}) {
        for (int s = 0; s < kStatusCount; ++s) {
            os << "cortrix_tasks_completed_total{task_type=\"" << ToString(tt)
               << "\",status=\"" << ToString(static_cast<CompletionStatus>(s)) << "\"} "
               << CompletedCount(tt, static_cast<CompletionStatus>(s)) << "\n";
        }
    }

    // cortrix_tasks_duration_seconds (histogram, label: task_type) — proper
    // Prometheus histogram per task_type: cumulative _bucket{le=...} + le="+Inf",
    // then _sum + _count. Only emit task_types that have observations.
    os << "# HELP cortrix_tasks_duration_seconds Task execution latency in seconds.\n";
    os << "# TYPE cortrix_tasks_duration_seconds histogram\n";
    for (TaskType tt : {kTaskDocParse, kTaskWatcherFanout, kTaskDocSummary}) {
        const Hist& h = duration_[TaskTypeIndex(tt)];
        const uint64_t cnt = h.count.load(std::memory_order_relaxed);
        if (cnt == 0) continue;
        const char* label = ToString(tt);
        uint64_t cum = 0;
        for (int b = 0; b < kNumDurBuckets; ++b) {
            cum += h.bkt[b].load(std::memory_order_relaxed);
            os << "cortrix_tasks_duration_seconds_bucket{task_type=\"" << label
               << "\",le=\"" << kDurBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += h.bkt[kNumDurBuckets].load(std::memory_order_relaxed);
        os << "cortrix_tasks_duration_seconds_bucket{task_type=\"" << label
           << "\",le=\"+Inf\"} " << cum << "\n";
        const double sum_s =
            static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1e6;
        os << "cortrix_tasks_duration_seconds_sum{task_type=\"" << label << "\"} "
           << sum_s << "\n";
        os << "cortrix_tasks_duration_seconds_count{task_type=\"" << label << "\"} "
           << cnt << "\n";
    }

    // cortrix_tasks_queue_depth (gauge, label: state)
    os << "# HELP cortrix_tasks_queue_depth Current task queue depth by state.\n";
    os << "# TYPE cortrix_tasks_queue_depth gauge\n";
    for (int s = 0; s < kStateCount; ++s) {
        os << "cortrix_tasks_queue_depth{state=\"" << ToString(static_cast<QueueState>(s))
           << "\"} " << queue_depth_[s].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_tasks_zombie_cleaned_total (counter, no label)
    os << "# HELP cortrix_tasks_zombie_cleaned_total Zombie tasks swept to failed by the cleanup cron.\n";
    os << "# TYPE cortrix_tasks_zombie_cleaned_total counter\n";
    os << "cortrix_tasks_zombie_cleaned_total "
       << zombie_cleaned_.load(std::memory_order_relaxed) << "\n";

    // cortrix_tasks_cancel_total (counter, label: phase)
    os << "# HELP cortrix_tasks_cancel_total Task cancellations by lifecycle phase.\n";
    os << "# TYPE cortrix_tasks_cancel_total counter\n";
    for (int p = 0; p < kPhaseCount; ++p) {
        os << "cortrix_tasks_cancel_total{phase=\"" << ToString(static_cast<CancelPhase>(p))
           << "\"} " << cancel_[p].load(std::memory_order_relaxed) << "\n";
    }

    return os.str();
}

void TaskMetrics::ResetForTest() {
    for (auto& a : submitted_) a.store(0, std::memory_order_relaxed);
    for (auto& row : completed_)
        for (auto& a : row) a.store(0, std::memory_order_relaxed);
    for (auto& h : duration_) {
        for (auto& b : h.bkt) b.store(0, std::memory_order_relaxed);
        h.sum_us.store(0, std::memory_order_relaxed);
        h.count.store(0, std::memory_order_relaxed);
    }
    for (auto& a : queue_depth_) a.store(0, std::memory_order_relaxed);
    zombie_cleaned_.store(0, std::memory_order_relaxed);
    for (auto& a : cancel_) a.store(0, std::memory_order_relaxed);
}

const char* ToString(TaskType task_type) {
    switch (task_type) {
        case kTaskDocParse:      return "kTaskDocParse";
        case kTaskWatcherFanout: return "kTaskWatcherFanout";
        case kTaskDocSummary:    return "kTaskDocSummary";
    }
    return "kTaskDocParse";
}

const char* ToString(TaskMetrics::CompletionStatus status) {
    switch (status) {
        case TaskMetrics::CompletionStatus::kSuccess:   return "success";
        case TaskMetrics::CompletionStatus::kFailed:    return "failed";
        case TaskMetrics::CompletionStatus::kCancelled: return "cancelled";
        case TaskMetrics::CompletionStatus::kTimeout:   return "timeout";
    }
    return "unknown";
}

const char* ToString(TaskMetrics::QueueState state) {
    switch (state) {
        case TaskMetrics::QueueState::kQueued:     return "queued";
        case TaskMetrics::QueueState::kProcessing: return "processing";
    }
    return "unknown";
}

const char* ToString(TaskMetrics::CancelPhase phase) {
    switch (phase) {
        case TaskMetrics::CancelPhase::kPreDequeue:    return "pre_dequeue";
        case TaskMetrics::CancelPhase::kMidProcessing: return "mid_processing";
        case TaskMetrics::CancelPhase::kPostChunkIdx:  return "post_chunk_idx";
    }
    return "unknown";
}

}  // namespace cortrix::async
