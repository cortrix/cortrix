#include <cstdint>
#include "cortrix/resource/ns_pool_metrics.h"

#include <sstream>

namespace cortrix::resource {

namespace {

// Duration histogram bucket upper bounds (seconds), shared by both ns_pool
// duration histograms. The perf target assumes ~3s real IO per NS load and
// startup loads up to max_namespaces concurrently → bounds straddle a few seconds
// (per-NS) up to a minute (whole-startup tail).
constexpr double kDurBounds[NsPoolMetrics::kNumDurBuckets] =
    {0.1, 0.5, 1.0, 3.0, 5.0, 10.0, 30.0, 60.0};
constexpr const char* kDurBoundStr[NsPoolMetrics::kNumDurBuckets] =
    {"0.1", "0.5", "1", "3", "5", "10", "30", "60"};

// Index of the bucket a `seconds` observation falls into (kNumDurBuckets = +Inf).
int DurBucketIndex(double seconds) {
    for (int j = 0; j < NsPoolMetrics::kNumDurBuckets; ++j) {
        if (seconds <= kDurBounds[j]) return j;
    }
    return NsPoolMetrics::kNumDurBuckets;  // +Inf
}

}  // namespace

NsPoolMetrics& NsPoolMetrics::Instance() {
    static NsPoolMetrics instance;
    return instance;
}

void NsPoolMetrics::SetSize(int64_t size) {
    if (size < 0) size = 0;
    size_.store(size, std::memory_order_relaxed);
}

int64_t NsPoolMetrics::Size() const {
    return size_.load(std::memory_order_relaxed);
}

void NsPoolMetrics::SetMemoryBudgetUsedBytes(int64_t bytes) {
    if (bytes < 0) bytes = 0;
    memory_budget_used_bytes_.store(bytes, std::memory_order_relaxed);
}

int64_t NsPoolMetrics::MemoryBudgetUsedBytes() const {
    return memory_budget_used_bytes_.load(std::memory_order_relaxed);
}

void NsPoolMetrics::RecordRejectedCreate(RejectReason reason) {
    rejected_creates_[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t NsPoolMetrics::RejectedCreateCount(RejectReason reason) const {
    return rejected_creates_[static_cast<int>(reason)].load(std::memory_order_relaxed);
}

void NsPoolMetrics::AddStartupLoadFailures(uint64_t n) {
    startup_load_failures_.fetch_add(n, std::memory_order_relaxed);
}

uint64_t NsPoolMetrics::StartupLoadFailuresCount() const {
    return startup_load_failures_.load(std::memory_order_relaxed);
}

void NsPoolMetrics::ObserveStartupLoadDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    startup_load_dur_.bkt[DurBucketIndex(seconds)].fetch_add(1, std::memory_order_relaxed);
    startup_load_dur_.count.fetch_add(1, std::memory_order_relaxed);
    startup_load_dur_.sum_us.fetch_add(static_cast<uint64_t>(seconds * 1e6),
                                       std::memory_order_relaxed);
}

void NsPoolMetrics::ObserveNsLoadDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    ns_load_dur_.bkt[DurBucketIndex(seconds)].fetch_add(1, std::memory_order_relaxed);
    ns_load_dur_.count.fetch_add(1, std::memory_order_relaxed);
    ns_load_dur_.sum_us.fetch_add(static_cast<uint64_t>(seconds * 1e6),
                                  std::memory_order_relaxed);
}

void NsPoolMetrics::RenderHist(std::ostream& os, const char* name, const Hist& h) {
    uint64_t cum = 0;
    for (int b = 0; b < kNumDurBuckets; ++b) {
        cum += h.bkt[b].load(std::memory_order_relaxed);
        os << name << "_bucket{le=\"" << kDurBoundStr[b] << "\"} " << cum << "\n";
    }
    cum += h.bkt[kNumDurBuckets].load(std::memory_order_relaxed);
    os << name << "_bucket{le=\"+Inf\"} " << cum << "\n";
    os << name << "_sum "
       << (static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1e6) << "\n";
    os << name << "_count " << h.count.load(std::memory_order_relaxed) << "\n";
}

std::string NsPoolMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_ns_pool_size (gauge)
    os << "# HELP cortrix_ns_pool_size Current resident namespace count.\n";
    os << "# TYPE cortrix_ns_pool_size gauge\n";
    os << "cortrix_ns_pool_size " << size_.load(std::memory_order_relaxed) << "\n";

    // cortrix_ns_pool_memory_budget_used_bytes (gauge)
    os << "# HELP cortrix_ns_pool_memory_budget_used_bytes Estimated resident-pool memory in bytes.\n";
    os << "# TYPE cortrix_ns_pool_memory_budget_used_bytes gauge\n";
    os << "cortrix_ns_pool_memory_budget_used_bytes "
       << memory_budget_used_bytes_.load(std::memory_order_relaxed) << "\n";

    // cortrix_ns_pool_rejected_creates_total (counter, label: reason)
    os << "# HELP cortrix_ns_pool_rejected_creates_total NS creates rejected by admission control.\n";
    os << "# TYPE cortrix_ns_pool_rejected_creates_total counter\n";
    for (int r = 0; r < kReasonCount; ++r) {
        os << "cortrix_ns_pool_rejected_creates_total{reason=\""
           << ToString(static_cast<RejectReason>(r)) << "\"} "
           << rejected_creates_[r].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_ns_pool_startup_load_failures_total (counter)
    os << "# HELP cortrix_ns_pool_startup_load_failures_total Namespaces that failed to load at startup.\n";
    os << "# TYPE cortrix_ns_pool_startup_load_failures_total counter\n";
    os << "cortrix_ns_pool_startup_load_failures_total "
       << startup_load_failures_.load(std::memory_order_relaxed) << "\n";

    // cortrix_ns_pool_startup_load_duration_seconds (histogram)
    os << "# HELP cortrix_ns_pool_startup_load_duration_seconds Whole-pool startup load duration.\n";
    os << "# TYPE cortrix_ns_pool_startup_load_duration_seconds histogram\n";
    RenderHist(os, "cortrix_ns_pool_startup_load_duration_seconds", startup_load_dur_);

    // cortrix_ns_pool_ns_load_duration_seconds (histogram)
    os << "# HELP cortrix_ns_pool_ns_load_duration_seconds Per-namespace load latency.\n";
    os << "# TYPE cortrix_ns_pool_ns_load_duration_seconds histogram\n";
    RenderHist(os, "cortrix_ns_pool_ns_load_duration_seconds", ns_load_dur_);

    return os.str();
}

void NsPoolMetrics::ResetForTest() {
    size_.store(0, std::memory_order_relaxed);
    memory_budget_used_bytes_.store(0, std::memory_order_relaxed);
    for (auto& a : rejected_creates_) a.store(0, std::memory_order_relaxed);
    startup_load_failures_.store(0, std::memory_order_relaxed);
    for (auto* h : {&startup_load_dur_, &ns_load_dur_}) {
        for (auto& b : h->bkt) b.store(0, std::memory_order_relaxed);
        h->sum_us.store(0, std::memory_order_relaxed);
        h->count.store(0, std::memory_order_relaxed);
    }
}

const char* ToString(NsPoolMetrics::RejectReason reason) {
    switch (reason) {
        case NsPoolMetrics::RejectReason::kNsCountExceeded: return "ns_count_exceeded";
        case NsPoolMetrics::RejectReason::kMemoryExceeded:  return "memory_exceeded";
    }
    return "unknown";
}

}  // namespace cortrix::resource
