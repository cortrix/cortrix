#include "cortrix/observability/oplog_metrics.h"

#include <sstream>

namespace cortrix::observability {

namespace {

// query_latency_seconds bucket upper bounds (seconds), §11. API query latencies
// are sub-second, so the bounds mirror the F13 trace-query histogram.
constexpr double kQBounds[OplogMetrics::kNumQueryBuckets] =
    {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0};
constexpr const char* kQBoundStr[OplogMetrics::kNumQueryBuckets] =
    {"0.005", "0.01", "0.025", "0.05", "0.1", "0.25", "0.5", "1"};

// cleanup_duration_seconds bucket upper bounds (seconds), §11. A cleanup sweep is a
// SQLite DELETE over the retention window / row-cap, so the bounds run wider.
constexpr double kClnBounds[OplogMetrics::kNumCleanupBuckets] =
    {0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 30.0};
constexpr const char* kClnBoundStr[OplogMetrics::kNumCleanupBuckets] =
    {"0.01", "0.05", "0.1", "0.5", "1", "5", "10", "30"};

int BucketIndex(double seconds, const double* bounds, int n) {
    for (int j = 0; j < n; ++j) {
        if (seconds <= bounds[j]) return j;
    }
    return n;  // +Inf
}

}  // namespace

OplogMetrics& OplogMetrics::Instance() {
    static OplogMetrics instance;
    return instance;
}

void OplogMetrics::RecordWrite(const std::string& action,
                               const std::string& resource_type) {
    std::lock_guard<std::mutex> lock(writes_mu_);
    ++writes_[{action, resource_type}];
}

uint64_t OplogMetrics::WriteCount(const std::string& action,
                                  const std::string& resource_type) const {
    std::lock_guard<std::mutex> lock(writes_mu_);
    auto it = writes_.find({action, resource_type});
    return it == writes_.end() ? 0 : it->second;
}

void OplogMetrics::ObserveQueryLatency(int filter_dimensions, int latency_ms) {
    if (filter_dimensions < 0) filter_dimensions = 0;
    if (filter_dimensions > kMaxFilterDimensions) filter_dimensions = kMaxFilterDimensions;
    if (latency_ms < 0) latency_ms = 0;
    Histo& h = q_lat_[filter_dimensions];
    h.sum_ms.fetch_add(static_cast<uint64_t>(latency_ms), std::memory_order_relaxed);
    h.count.fetch_add(1, std::memory_order_relaxed);
    const int bi = BucketIndex(static_cast<double>(latency_ms) / 1000.0,
                               kQBounds, kNumQueryBuckets);
    h.bkt[bi].fetch_add(1, std::memory_order_relaxed);
}

uint64_t OplogMetrics::QueryLatencyCount(int filter_dimensions) const {
    if (filter_dimensions < 0) filter_dimensions = 0;
    if (filter_dimensions > kMaxFilterDimensions) filter_dimensions = kMaxFilterDimensions;
    return q_lat_[filter_dimensions].count.load(std::memory_order_relaxed);
}

void OplogMetrics::RecordCleanupDeleted(CleanupReason reason, int64_t count) {
    if (count <= 0) return;
    cleanup_deleted_[static_cast<int>(reason)].fetch_add(
        static_cast<uint64_t>(count), std::memory_order_relaxed);
}

uint64_t OplogMetrics::CleanupDeletedCount(CleanupReason reason) const {
    return cleanup_deleted_[static_cast<int>(reason)].load(std::memory_order_relaxed);
}

void OplogMetrics::ObserveCleanupDuration(int duration_ms) {
    if (duration_ms < 0) duration_ms = 0;
    cln_sum_ms_.fetch_add(static_cast<uint64_t>(duration_ms), std::memory_order_relaxed);
    cln_count_.fetch_add(1, std::memory_order_relaxed);
    const int bi = BucketIndex(static_cast<double>(duration_ms) / 1000.0,
                               kClnBounds, kNumCleanupBuckets);
    cln_bkt_[bi].fetch_add(1, std::memory_order_relaxed);
}

uint64_t OplogMetrics::CleanupDurationCount() const {
    return cln_count_.load(std::memory_order_relaxed);
}

void OplogMetrics::RecordCleanupFailed() {
    cleanup_failed_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t OplogMetrics::CleanupFailedCount() const {
    return cleanup_failed_.load(std::memory_order_relaxed);
}

void OplogMetrics::SetSizeRows(int64_t rows) {
    size_rows_.store(rows, std::memory_order_relaxed);
}

int64_t OplogMetrics::SizeRows() const {
    return size_rows_.load(std::memory_order_relaxed);
}

std::string OplogMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_oplog_writes_total {action, resource_type}
    os << "# HELP cortrix_oplog_writes_total Total operation_log writes by action and resource_type.\n";
    os << "# TYPE cortrix_oplog_writes_total counter\n";
    {
        std::lock_guard<std::mutex> lock(writes_mu_);
        for (const auto& kv : writes_) {
            os << "cortrix_oplog_writes_total{action=\"" << kv.first.first
               << "\",resource_type=\"" << kv.first.second << "\"} " << kv.second << "\n";
        }
    }

    // cortrix_oplog_query_latency_seconds (histogram, label: filter_dimensions)
    os << "# HELP cortrix_oplog_query_latency_seconds operation_log query latency in seconds.\n";
    os << "# TYPE cortrix_oplog_query_latency_seconds histogram\n";
    for (int d = 0; d < kFilterDimensionSeries; ++d) {
        const Histo& h = q_lat_[d];
        // Skip series with no observations to keep the exposition compact.
        if (h.count.load(std::memory_order_relaxed) == 0) continue;
        const std::string dim = std::to_string(d);
        uint64_t cum = 0;
        for (int b = 0; b < kNumQueryBuckets; ++b) {
            cum += h.bkt[b].load(std::memory_order_relaxed);
            os << "cortrix_oplog_query_latency_seconds_bucket{filter_dimensions=\""
               << dim << "\",le=\"" << kQBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += h.bkt[kNumQueryBuckets].load(std::memory_order_relaxed);
        os << "cortrix_oplog_query_latency_seconds_bucket{filter_dimensions=\""
           << dim << "\",le=\"+Inf\"} " << cum << "\n";
        const double sum_s =
            static_cast<double>(h.sum_ms.load(std::memory_order_relaxed)) / 1000.0;
        os << "cortrix_oplog_query_latency_seconds_sum{filter_dimensions=\""
           << dim << "\"} " << sum_s << "\n";
        os << "cortrix_oplog_query_latency_seconds_count{filter_dimensions=\""
           << dim << "\"} " << h.count.load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_oplog_cleanup_deleted_total {reason}
    os << "# HELP cortrix_oplog_cleanup_deleted_total operation_log rows deleted by cleanup, by reason.\n";
    os << "# TYPE cortrix_oplog_cleanup_deleted_total counter\n";
    for (int r = 0; r < kCleanupReasonCount; ++r) {
        os << "cortrix_oplog_cleanup_deleted_total{reason=\""
           << ToString(static_cast<CleanupReason>(r)) << "\"} "
           << cleanup_deleted_[r].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_oplog_cleanup_duration_seconds (histogram, no label)
    os << "# HELP cortrix_oplog_cleanup_duration_seconds operation_log cleanup sweep duration in seconds.\n";
    os << "# TYPE cortrix_oplog_cleanup_duration_seconds histogram\n";
    {
        uint64_t cum = 0;
        for (int b = 0; b < kNumCleanupBuckets; ++b) {
            cum += cln_bkt_[b].load(std::memory_order_relaxed);
            os << "cortrix_oplog_cleanup_duration_seconds_bucket{le=\""
               << kClnBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += cln_bkt_[kNumCleanupBuckets].load(std::memory_order_relaxed);
        os << "cortrix_oplog_cleanup_duration_seconds_bucket{le=\"+Inf\"} " << cum << "\n";
        const double sum_s =
            static_cast<double>(cln_sum_ms_.load(std::memory_order_relaxed)) / 1000.0;
        os << "cortrix_oplog_cleanup_duration_seconds_sum " << sum_s << "\n";
        os << "cortrix_oplog_cleanup_duration_seconds_count "
           << cln_count_.load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_oplog_cleanup_failed_total (counter, no label)
    os << "# HELP cortrix_oplog_cleanup_failed_total operation_log cleanup sweep failures.\n";
    os << "# TYPE cortrix_oplog_cleanup_failed_total counter\n";
    os << "cortrix_oplog_cleanup_failed_total "
       << cleanup_failed_.load(std::memory_order_relaxed) << "\n";

    // cortrix_oplog_size_rows (gauge, no label)
    os << "# HELP cortrix_oplog_size_rows Current operation_log row count.\n";
    os << "# TYPE cortrix_oplog_size_rows gauge\n";
    os << "cortrix_oplog_size_rows " << size_rows_.load(std::memory_order_relaxed) << "\n";

    return os.str();
}

void OplogMetrics::ResetForTest() {
    {
        std::lock_guard<std::mutex> lock(writes_mu_);
        writes_.clear();
    }
    for (auto& a : cleanup_deleted_) a.store(0, std::memory_order_relaxed);
    cleanup_failed_.store(0, std::memory_order_relaxed);
    size_rows_.store(0, std::memory_order_relaxed);
    for (auto& h : q_lat_) {
        h.sum_ms.store(0, std::memory_order_relaxed);
        h.count.store(0, std::memory_order_relaxed);
        for (auto& b : h.bkt) b.store(0, std::memory_order_relaxed);
    }
    cln_sum_ms_.store(0, std::memory_order_relaxed);
    cln_count_.store(0, std::memory_order_relaxed);
    for (auto& b : cln_bkt_) b.store(0, std::memory_order_relaxed);
}

const char* ToString(OplogMetrics::CleanupReason reason) {
    switch (reason) {
        case OplogMetrics::CleanupReason::kAge:   return "age";
        case OplogMetrics::CleanupReason::kQuota: return "quota";
    }
    return "unknown";
}

}  // namespace cortrix::observability
