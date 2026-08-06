#include <cstdint>
#include "cortrix/store/parent_chunk_store_metrics.h"

#include <sstream>

namespace cortrix::store {

namespace {

// lookup_latency_seconds histogram bucket upper bounds (seconds), Parallel
// string array for rendering exact `le` labels. The trailing +Inf bucket is
// implicit (index kNumDurBuckets). A parent lookup is a single indexed SQLite
// point query, so the bounds are sub-millisecond-centric.
constexpr double kDurBounds[ParentChunkStoreMetrics::kNumDurBuckets] =
    {0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.05};
constexpr const char* kDurBoundStr[ParentChunkStoreMetrics::kNumDurBuckets] =
    {"0.0001", "0.00025", "0.0005", "0.001", "0.0025", "0.005", "0.01", "0.05"};

int DurBucketIndex(double seconds) {
    for (int j = 0; j < ParentChunkStoreMetrics::kNumDurBuckets; ++j) {
        if (seconds <= kDurBounds[j]) return j;
    }
    return ParentChunkStoreMetrics::kNumDurBuckets;  // +Inf
}

}  // namespace

ParentChunkStoreMetrics& ParentChunkStoreMetrics::Instance() {
    static ParentChunkStoreMetrics instance;
    return instance;
}

void ParentChunkStoreMetrics::RecordLookup(LookupResult result) {
    lookup_[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ParentChunkStoreMetrics::LookupCount(LookupResult result) const {
    return lookup_[static_cast<int>(result)].load(std::memory_order_relaxed);
}

void ParentChunkStoreMetrics::ObserveLookupLatency(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    lat_sum_us_.fetch_add(static_cast<uint64_t>(seconds * 1e6), std::memory_order_relaxed);
    lat_count_.fetch_add(1, std::memory_order_relaxed);
    const int bi = DurBucketIndex(seconds);
    lat_bkt_[bi].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ParentChunkStoreMetrics::LookupLatencyCount() const {
    return lat_count_.load(std::memory_order_relaxed);
}

double ParentChunkStoreMetrics::LookupLatencySum() const {
    return static_cast<double>(lat_sum_us_.load(std::memory_order_relaxed)) / 1e6;
}

std::string ParentChunkStoreMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_parent_chunk_store_lookup_total {result}
    os << "# HELP cortrix_parent_chunk_store_lookup_total Parent reverse-lookups by result.\n";
    os << "# TYPE cortrix_parent_chunk_store_lookup_total counter\n";
    for (int r = 0; r < kLookupResultCount; ++r) {
        os << "cortrix_parent_chunk_store_lookup_total{result=\""
           << ToString(static_cast<LookupResult>(r)) << "\"} "
           << lookup_[r].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_parent_chunk_store_lookup_latency_seconds (histogram, no label)
    os << "# HELP cortrix_parent_chunk_store_lookup_latency_seconds Parent lookup latency in seconds.\n";
    os << "# TYPE cortrix_parent_chunk_store_lookup_latency_seconds histogram\n";
    {
        uint64_t cum = 0;
        for (int b = 0; b < kNumDurBuckets; ++b) {
            cum += lat_bkt_[b].load(std::memory_order_relaxed);
            os << "cortrix_parent_chunk_store_lookup_latency_seconds_bucket{le=\""
               << kDurBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += lat_bkt_[kNumDurBuckets].load(std::memory_order_relaxed);
        os << "cortrix_parent_chunk_store_lookup_latency_seconds_bucket{le=\"+Inf\"} " << cum << "\n";
        const double sum_s =
            static_cast<double>(lat_sum_us_.load(std::memory_order_relaxed)) / 1e6;
        os << "cortrix_parent_chunk_store_lookup_latency_seconds_sum " << sum_s << "\n";
        os << "cortrix_parent_chunk_store_lookup_latency_seconds_count "
           << lat_count_.load(std::memory_order_relaxed) << "\n";
    }

    return os.str();
}

void ParentChunkStoreMetrics::ResetForTest() {
    for (auto& a : lookup_) a.store(0, std::memory_order_relaxed);
    lat_sum_us_.store(0, std::memory_order_relaxed);
    lat_count_.store(0, std::memory_order_relaxed);
    for (auto& b : lat_bkt_) b.store(0, std::memory_order_relaxed);
}

const char* ToString(ParentChunkStoreMetrics::LookupResult result) {
    switch (result) {
        case ParentChunkStoreMetrics::LookupResult::kHit:  return "hit";
        case ParentChunkStoreMetrics::LookupResult::kMiss: return "miss";
    }
    return "unknown";
}

}  // namespace cortrix::store
