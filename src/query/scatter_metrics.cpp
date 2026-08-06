#include <cstdint>
#include "cortrix/query/scatter_metrics.h"

#include <sstream>

namespace cortrix::query {

using agent_friendly::ErrorCategory;

namespace {

// namespaces_per_query `le` bounds — count-type, aligned to the SLA NS
// tiers (1 / 3 / 10 / 100). kNsBuckets finite bounds + a trailing +Inf.
constexpr double kNsBounds[] = {1, 3, 10, 100};
constexpr const char* kNsBoundStr[] = {"1", "3", "10", "100"};

// duration_seconds `le` latency bounds (seconds) — generic duration set that
// straddles the target/max SLA tiers (0.5/0.6/1.0/5.0s target,
// 1.5/1.8/3.0/15.0s max). kLatencyBuckets finite bounds + a trailing +Inf.
constexpr double kLatBounds[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30};
constexpr const char* kLatBoundStr[] = {"0.1", "0.25", "0.5", "1", "2", "5", "10", "30"};

// Index of the first finite bucket whose bound ≥ value, else the +Inf slot.
template <size_t N>
int LeIndex(double value, const double (&bounds)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (value <= bounds[i]) return static_cast<int>(i);
    }
    return static_cast<int>(N);  // +Inf
}

}  // namespace

ScatterMetrics& ScatterMetrics::Instance() {
    static ScatterMetrics instance;
    return instance;
}

int ScatterMetrics::Idx(Reason r, ErrorCategory c) {
    return static_cast<int>(r) * kCategoryCount + static_cast<int>(c);
}

// --- requests_total / failed_total ---

void ScatterMetrics::RecordRequest(Reason reason, ErrorCategory category) {
    requests_[Idx(reason, category)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScatterMetrics::RequestCount(Reason reason, ErrorCategory category) const {
    return requests_[Idx(reason, category)].load(std::memory_order_relaxed);
}

void ScatterMetrics::RecordFailed(Reason reason, ErrorCategory category) {
    failed_[Idx(reason, category)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScatterMetrics::FailedCount(Reason reason, ErrorCategory category) const {
    return failed_[Idx(reason, category)].load(std::memory_order_relaxed);
}

// --- namespaces_per_query (histogram: sum + count) ---

void ScatterMetrics::ObserveNamespacesPerQuery(int n) {
    if (n < 0) n = 0;
    ns_per_query_sum_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    ns_per_query_count_.fetch_add(1, std::memory_order_relaxed);
    ns_per_query_bkt_[LeIndex(n, kNsBounds)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScatterMetrics::NamespacesPerQuerySum() const {
    return ns_per_query_sum_.load(std::memory_order_relaxed);
}

uint64_t ScatterMetrics::NamespacesPerQueryCount() const {
    return ns_per_query_count_.load(std::memory_order_relaxed);
}

uint64_t ScatterMetrics::NamespacesPerQueryBucket(int bucket) const {
    if (bucket < 0 || bucket > kNsBuckets) return 0;
    return ns_per_query_bkt_[bucket].load(std::memory_order_relaxed);
}

// --- dedup_collisions_total ---

void ScatterMetrics::RecordDedupCollisions(uint64_t collapsed) {
    dedup_collisions_.fetch_add(collapsed, std::memory_order_relaxed);
}

uint64_t ScatterMetrics::DedupCollisionsCount() const {
    return dedup_collisions_.load(std::memory_order_relaxed);
}

// --- partial_success_total ---

void ScatterMetrics::RecordPartialSuccess() {
    partial_success_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScatterMetrics::PartialSuccessCount() const {
    return partial_success_.load(std::memory_order_relaxed);
}

// --- duration_seconds (histogram by NS-count bucket) ---

int ScatterMetrics::DurationBucket(int namespace_count) {
    if (namespace_count <= 1) return 0;
    if (namespace_count <= 3) return 1;
    if (namespace_count <= 10) return 2;
    return 3;
}

void ScatterMetrics::ObserveDuration(int namespace_count, int latency_ms) {
    if (latency_ms < 0) latency_ms = 0;
    const int b = DurationBucket(namespace_count);
    duration_sum_ms_[b].fetch_add(static_cast<uint64_t>(latency_ms),
                                  std::memory_order_relaxed);
    duration_count_[b].fetch_add(1, std::memory_order_relaxed);
    const int le = LeIndex(latency_ms / 1000.0, kLatBounds);
    duration_lat_bkt_[b][le].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScatterMetrics::DurationSumMs(int bucket) const {
    if (bucket < 0 || bucket >= kDurationBuckets) return 0;
    return duration_sum_ms_[bucket].load(std::memory_order_relaxed);
}

uint64_t ScatterMetrics::DurationCount(int bucket) const {
    if (bucket < 0 || bucket >= kDurationBuckets) return 0;
    return duration_count_[bucket].load(std::memory_order_relaxed);
}

uint64_t ScatterMetrics::DurationLatencyBucket(int ns_bucket, int le_index) const {
    if (ns_bucket < 0 || ns_bucket >= kDurationBuckets) return 0;
    if (le_index < 0 || le_index > kLatencyBuckets) return 0;
    return duration_lat_bkt_[ns_bucket][le_index].load(std::memory_order_relaxed);
}

// --- OpenMetrics rendering ---

namespace {
const char* BucketLabel(int b) {
    switch (b) {
        case 0: return "1";
        case 1: return "3";
        case 2: return "10";
        default: return "100";
    }
}
}  // namespace

std::string ScatterMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    os << "# TYPE cortrix_scatter_requests_total counter\n";
    for (int r = 0; r < kReasonCount; ++r) {
        for (int c = 0; c < kCategoryCount; ++c) {
            os << "cortrix_scatter_requests_total{reason=\""
               << ToString(static_cast<Reason>(r)) << "\",category=\""
               << agent_friendly::ToString(static_cast<ErrorCategory>(c)) << "\"} "
               << requests_[r * kCategoryCount + c].load(std::memory_order_relaxed)
               << "\n";
        }
    }

    os << "# TYPE cortrix_scatter_failed_total counter\n";
    for (int r = 0; r < kReasonCount; ++r) {
        for (int c = 0; c < kCategoryCount; ++c) {
            os << "cortrix_scatter_failed_total{reason=\""
               << ToString(static_cast<Reason>(r)) << "\",category=\""
               << agent_friendly::ToString(static_cast<ErrorCategory>(c)) << "\"} "
               << failed_[r * kCategoryCount + c].load(std::memory_order_relaxed)
               << "\n";
        }
    }

    os << "# TYPE cortrix_scatter_namespaces_per_query histogram\n";
    {
        uint64_t cum = 0;
        for (int i = 0; i < kNsBuckets; ++i) {
            cum += ns_per_query_bkt_[i].load(std::memory_order_relaxed);
            os << "cortrix_scatter_namespaces_per_query_bucket{le=\"" << kNsBoundStr[i]
               << "\"} " << cum << "\n";
        }
        cum += ns_per_query_bkt_[kNsBuckets].load(std::memory_order_relaxed);
        os << "cortrix_scatter_namespaces_per_query_bucket{le=\"+Inf\"} " << cum << "\n";
    }
    os << "cortrix_scatter_namespaces_per_query_sum "
       << ns_per_query_sum_.load(std::memory_order_relaxed) << "\n";
    os << "cortrix_scatter_namespaces_per_query_count "
       << ns_per_query_count_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_scatter_dedup_collisions_total counter\n";
    os << "cortrix_scatter_dedup_collisions_total "
       << dedup_collisions_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_scatter_partial_success_total counter\n";
    os << "cortrix_scatter_partial_success_total "
       << partial_success_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_scatter_duration_seconds histogram\n";
    for (int b = 0; b < kDurationBuckets; ++b) {
        // Cumulative latency `le` buckets within this NS-count series.
        // Label order matches the codebase convention (other label first, le last —
        // cf. import_metrics RenderHist).
        uint64_t cum = 0;
        for (int i = 0; i < kLatencyBuckets; ++i) {
            cum += duration_lat_bkt_[b][i].load(std::memory_order_relaxed);
            os << "cortrix_scatter_duration_seconds_bucket{namespace_count_bucket=\""
               << BucketLabel(b) << "\",le=\"" << kLatBoundStr[i] << "\"} " << cum << "\n";
        }
        cum += duration_lat_bkt_[b][kLatencyBuckets].load(std::memory_order_relaxed);
        os << "cortrix_scatter_duration_seconds_bucket{namespace_count_bucket=\""
           << BucketLabel(b) << "\",le=\"+Inf\"} " << cum << "\n";
        os << "cortrix_scatter_duration_seconds_sum{namespace_count_bucket=\""
           << BucketLabel(b) << "\"} "
           << static_cast<double>(duration_sum_ms_[b].load(std::memory_order_relaxed)) /
                  1000.0
           << "\n";
        os << "cortrix_scatter_duration_seconds_count{namespace_count_bucket=\""
           << BucketLabel(b) << "\"} "
           << duration_count_[b].load(std::memory_order_relaxed) << "\n";
    }

    return os.str();
}

void ScatterMetrics::ResetForTest() {
    for (auto& a : requests_) a.store(0, std::memory_order_relaxed);
    for (auto& a : failed_) a.store(0, std::memory_order_relaxed);
    ns_per_query_sum_.store(0, std::memory_order_relaxed);
    ns_per_query_count_.store(0, std::memory_order_relaxed);
    for (auto& a : ns_per_query_bkt_) a.store(0, std::memory_order_relaxed);
    dedup_collisions_.store(0, std::memory_order_relaxed);
    partial_success_.store(0, std::memory_order_relaxed);
    for (auto& a : duration_sum_ms_) a.store(0, std::memory_order_relaxed);
    for (auto& a : duration_count_) a.store(0, std::memory_order_relaxed);
    for (auto& series : duration_lat_bkt_)
        for (auto& a : series) a.store(0, std::memory_order_relaxed);
}

const char* ToString(ScatterMetrics::Reason reason) {
    switch (reason) {
        case ScatterMetrics::Reason::kSingle:   return "single";
        case ScatterMetrics::Reason::kMulti:    return "multi";
        case ScatterMetrics::Reason::kWildcard: return "wildcard";
    }
    return "unknown";
}

}  // namespace cortrix::query
