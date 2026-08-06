#include <cstdint>
#include "cortrix/metadata/metadata_metrics.h"

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>

namespace cortrix::metadata {

namespace {

// Histogram bucket bounds (seconds) for generate_duration (§5.bis SLA P95 ≤ 50ms,
// P99 ≤ 200ms — buckets straddle those alarm thresholds).
constexpr size_t kNumDurBuckets = 6;
constexpr double kDurBounds[kNumDurBuckets] = {0.005, 0.01, 0.05, 0.1, 0.2, 1.0};
constexpr const char* kDurBoundStr[kNumDurBuckets] = {"0.005", "0.01", "0.05", "0.1", "0.2", "1"};

// Histogram bucket bounds (bytes) for metadata_size (26-field JSON + custom_metadata;
// observes custom_metadata size anomalies).
constexpr size_t kNumSizeBuckets = 6;
constexpr double kSizeBounds[kNumSizeBuckets] = {512, 1024, 2048, 4096, 8192, 65536};
constexpr const char* kSizeBoundStr[kNumSizeBuckets] = {"512", "1024", "2048", "4096", "8192", "65536"};

// Minimal lock-free Prometheus histogram (non-cumulative buckets; rendered cumulative).
template <size_t N>
struct Hist {
    std::array<std::atomic<uint64_t>, N + 1> bkt{};  // last = +Inf
    std::atomic<uint64_t> sum_milli{0};              // sum in 1/1000 units (ms or bytes*… see use)
    std::atomic<uint64_t> count{0};
    void Observe(double value, const double (&bounds)[N], double sum_scale) {
        if (value < 0) value = 0;
        size_t i = N;  // +Inf default
        for (size_t j = 0; j < N; ++j) {
            if (value <= bounds[j]) { i = j; break; }
        }
        bkt[i].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        sum_milli.fetch_add(static_cast<uint64_t>(value * sum_scale), std::memory_order_relaxed);
    }
    void Reset() {
        for (auto& b : bkt) b.store(0);
        sum_milli.store(0);
        count.store(0);
    }
};

// Process-global atomic state. Function-local statics → one set per process, zero-init.
struct State {
    // block_generated_total[status][source]
    std::array<std::array<std::atomic<uint64_t>, 3>, 3> generated{};
    std::atomic<int64_t> block_count{0};
    Hist<kNumDurBuckets> gen_dur{};
    Hist<kNumSizeBuckets> meta_size{};

    // field_missing_total{field_name} — bounded controlled enum, but keyed by string
    // so the generator can pass schema field names verbatim. Guarded by a mutex (the
    // write rate is low — once per missing field per partial generation).
    std::mutex field_mu;
    std::map<std::string, uint64_t> field_missing;
};

State& S() {
    static State s;
    return s;
}

template <size_t N>
void RenderHist(std::ostringstream& os, const char* name, const Hist<N>& h,
                const double (&)[N], const char* const (&bound_str)[N], double sum_scale) {
    uint64_t cum = 0;
    for (size_t i = 0; i < N; ++i) {
        cum += h.bkt[i].load(std::memory_order_relaxed);
        os << name << "_bucket{le=\"" << bound_str[i] << "\"} " << cum << "\n";
    }
    cum += h.bkt[N].load(std::memory_order_relaxed);
    os << name << "_bucket{le=\"+Inf\"} " << cum << "\n";
    os << name << "_sum " << (static_cast<double>(h.sum_milli.load(std::memory_order_relaxed)) / sum_scale) << "\n";
    os << name << "_count " << h.count.load(std::memory_order_relaxed) << "\n";
}

}  // namespace

const char* ToString(MetadataMetrics::GenStatus status) {
    switch (status) {
        case MetadataMetrics::GenStatus::kSuccess:        return "success";
        case MetadataMetrics::GenStatus::kPartialWarning: return "partial_warning";
        case MetadataMetrics::GenStatus::kFailed:         return "failed";
    }
    return "success";
}

const char* ToString(MetadataMetrics::GenSource source) {
    switch (source) {
        case MetadataMetrics::GenSource::kApiUpload:   return "api_upload";
        case MetadataMetrics::GenSource::kBatchImport: return "batch_import";
        case MetadataMetrics::GenSource::kReUpload:    return "re_upload";
    }
    return "api_upload";
}

MetadataMetrics& MetadataMetrics::Instance() {
    static MetadataMetrics inst;
    return inst;
}

void MetadataMetrics::RecordGenerated(GenStatus status, GenSource source) {
    S().generated[static_cast<size_t>(status)][static_cast<size_t>(source)]
        .fetch_add(1, std::memory_order_relaxed);
}

uint64_t MetadataMetrics::GeneratedCount(GenStatus status, GenSource source) const {
    return S().generated[static_cast<size_t>(status)][static_cast<size_t>(source)]
        .load(std::memory_order_relaxed);
}

void MetadataMetrics::SetBlockCount(int64_t count) {
    S().block_count.store(count, std::memory_order_relaxed);
}

int64_t MetadataMetrics::BlockCount() const {
    return S().block_count.load(std::memory_order_relaxed);
}

void MetadataMetrics::RecordFieldMissing(const std::string& field_name) {
    std::lock_guard<std::mutex> lk(S().field_mu);
    ++S().field_missing[field_name];
}

uint64_t MetadataMetrics::FieldMissingCount(const std::string& field_name) const {
    std::lock_guard<std::mutex> lk(S().field_mu);
    auto it = S().field_missing.find(field_name);
    return it == S().field_missing.end() ? 0 : it->second;
}

void MetadataMetrics::ObserveMetadataSize(double bytes) {
    S().meta_size.Observe(bytes, kSizeBounds, 1.0);  // sum in raw bytes
}

void MetadataMetrics::ObserveGenerateDuration(double seconds) {
    S().gen_dur.Observe(seconds, kDurBounds, 1e6);  // sum stored in microseconds
}

std::string MetadataMetrics::Render() const {
    std::ostringstream os;

    os << "# HELP cortrix_metadata_block_count_current Current metadata_blocks table row count.\n";
    os << "# TYPE cortrix_metadata_block_count_current gauge\n";
    os << "cortrix_metadata_block_count_current " << BlockCount() << "\n";

    os << "# HELP cortrix_metadata_block_generated_total Metadata block generations by status/source.\n";
    os << "# TYPE cortrix_metadata_block_generated_total counter\n";
    for (auto st : {GenStatus::kSuccess, GenStatus::kPartialWarning, GenStatus::kFailed}) {
        for (auto sr : {GenSource::kApiUpload, GenSource::kBatchImport, GenSource::kReUpload}) {
            os << "cortrix_metadata_block_generated_total{status=\"" << ToString(st)
               << "\",source=\"" << ToString(sr) << "\"} " << GeneratedCount(st, sr) << "\n";
        }
    }

    os << "# HELP cortrix_metadata_field_missing_total Field-missing count by field_name.\n";
    os << "# TYPE cortrix_metadata_field_missing_total counter\n";
    {
        std::lock_guard<std::mutex> lk(S().field_mu);
        for (const auto& [field, count] : S().field_missing) {
            os << "cortrix_metadata_field_missing_total{field_name=\"" << field << "\"} " << count << "\n";
        }
    }

    os << "# HELP cortrix_metadata_size_bytes metadata_json byte-size distribution.\n";
    os << "# TYPE cortrix_metadata_size_bytes histogram\n";
    RenderHist(os, "cortrix_metadata_size_bytes", S().meta_size, kSizeBounds, kSizeBoundStr, 1.0);

    os << "# HELP cortrix_metadata_block_generate_duration_seconds block_text assembly + JSON serialization time.\n";
    os << "# TYPE cortrix_metadata_block_generate_duration_seconds histogram\n";
    RenderHist(os, "cortrix_metadata_block_generate_duration_seconds", S().gen_dur, kDurBounds, kDurBoundStr, 1e6);

    return os.str();
}

void MetadataMetrics::ResetForTest() {
    for (auto& row : S().generated)
        for (auto& c : row) c.store(0);
    S().block_count.store(0);
    S().gen_dur.Reset();
    S().meta_size.Reset();
    std::lock_guard<std::mutex> lk(S().field_mu);
    S().field_missing.clear();
}

}  // namespace cortrix::metadata
