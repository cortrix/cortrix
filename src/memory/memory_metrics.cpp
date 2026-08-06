#include <cstdint>
#include "cortrix/memory/memory_metrics.h"

#include <sstream>

namespace cortrix::memory::transparency {

namespace {

// op_latency_seconds histogram bucket upper bounds (seconds), CRUD endpoints are
// sub-second metadata operations, so the bounds are tighter than the LLM-extraction
// histogram (memory_extract). Parallel string array renders exact `le` labels; the trailing
// +Inf bucket is implicit (index kNumDurBuckets). Mirrors the memory_extract kDurBounds template.
constexpr double kDurBounds[MemoryMetrics::kNumDurBuckets] =
    {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0};
constexpr const char* kDurBoundStr[MemoryMetrics::kNumDurBuckets] =
    {"0.005", "0.01", "0.025", "0.05", "0.1", "0.25", "0.5", "1"};

// Index of the bucket a `seconds` observation falls into (kNumDurBuckets = +Inf).
int DurBucketIndex(double seconds) {
    for (int j = 0; j < MemoryMetrics::kNumDurBuckets; ++j) {
        if (seconds <= kDurBounds[j]) return j;
    }
    return MemoryMetrics::kNumDurBuckets;  // +Inf
}

}  // namespace

MemoryMetrics& MemoryMetrics::Instance() {
    static MemoryMetrics instance;
    return instance;
}

void MemoryMetrics::RecordOp(Op op, OpStatus status) {
    op_total_[static_cast<int>(op)][static_cast<int>(status)].fetch_add(
        1, std::memory_order_relaxed);
}

uint64_t MemoryMetrics::OpCount(Op op, OpStatus status) const {
    return op_total_[static_cast<int>(op)][static_cast<int>(status)].load(
        std::memory_order_relaxed);
}

void MemoryMetrics::ObserveOpLatency(Op op, int latency_ms) {
    if (latency_ms < 0) latency_ms = 0;
    const int o = static_cast<int>(op);
    latency_sum_ms_[o].fetch_add(static_cast<uint64_t>(latency_ms),
                                 std::memory_order_relaxed);
    latency_count_[o].fetch_add(1, std::memory_order_relaxed);
    // Histogram: bump the (non-cumulative) bucket this observation falls into.
    const int bi = DurBucketIndex(static_cast<double>(latency_ms) / 1000.0);
    latency_bkt_[o][bi].fetch_add(1, std::memory_order_relaxed);
}

void MemoryMetrics::RecordCrossUserBlocked() {
    cross_user_blocked_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryMetrics::CrossUserBlockedCount() const {
    return cross_user_blocked_.load(std::memory_order_relaxed);
}

void MemoryMetrics::RecordEditConflict() {
    edit_conflict_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryMetrics::EditConflictCount() const {
    return edit_conflict_.load(std::memory_order_relaxed);
}

void MemoryMetrics::RecordInvalidInput(ErrorCodeLabel error_code) {
    invalid_input_[static_cast<int>(error_code)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryMetrics::InvalidInputCount(ErrorCodeLabel error_code) const {
    return invalid_input_[static_cast<int>(error_code)].load(std::memory_order_relaxed);
}

std::string MemoryMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_memory_transparency_op_total
    os << "# HELP cortrix_memory_transparency_op_total memory transparency operations by op and status.\n";
    os << "# TYPE cortrix_memory_transparency_op_total counter\n";
    for (int o = 0; o < kOpCount; ++o) {
        for (int s = 0; s < kOpStatusCount; ++s) {
            os << "cortrix_memory_transparency_op_total{op=\""
               << ToString(static_cast<Op>(o)) << "\",status=\""
               << ToString(static_cast<OpStatus>(s)) << "\"} "
               << op_total_[o][s].load(std::memory_order_relaxed) << "\n";
        }
    }

    // cortrix_memory_transparency_op_latency_seconds — Prometheus histogram per op:
    // cumulative _bucket{le=...} for each bound + le="+Inf", then _sum + _count.
    os << "# HELP cortrix_memory_transparency_op_latency_seconds memory transparency op latency in seconds.\n";
    os << "# TYPE cortrix_memory_transparency_op_latency_seconds histogram\n";
    for (int o = 0; o < kOpCount; ++o) {
        const uint64_t cnt = latency_count_[o].load(std::memory_order_relaxed);
        if (cnt == 0) continue;
        const char* label = ToString(static_cast<Op>(o));
        uint64_t cum = 0;
        for (int b = 0; b < kNumDurBuckets; ++b) {
            cum += latency_bkt_[o][b].load(std::memory_order_relaxed);
            os << "cortrix_memory_transparency_op_latency_seconds_bucket{op=\"" << label
               << "\",le=\"" << kDurBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += latency_bkt_[o][kNumDurBuckets].load(std::memory_order_relaxed);
        os << "cortrix_memory_transparency_op_latency_seconds_bucket{op=\"" << label
           << "\",le=\"+Inf\"} " << cum << "\n";
        const double sum_s =
            static_cast<double>(latency_sum_ms_[o].load(std::memory_order_relaxed)) / 1000.0;
        os << "cortrix_memory_transparency_op_latency_seconds_sum{op=\"" << label << "\"} "
           << sum_s << "\n";
        os << "cortrix_memory_transparency_op_latency_seconds_count{op=\"" << label << "\"} "
           << cnt << "\n";
    }

    // cortrix_memory_transparency_cross_user_blocked_total
    os << "# HELP cortrix_memory_transparency_cross_user_blocked_total Cross-user accesses masked as 404 (L1.bis).\n";
    os << "# TYPE cortrix_memory_transparency_cross_user_blocked_total counter\n";
    os << "cortrix_memory_transparency_cross_user_blocked_total "
       << cross_user_blocked_.load(std::memory_order_relaxed) << "\n";

    // cortrix_memory_transparency_edit_conflict_total
    os << "# HELP cortrix_memory_transparency_edit_conflict_total Optimistic-lock edit conflicts.\n";
    os << "# TYPE cortrix_memory_transparency_edit_conflict_total counter\n";
    os << "cortrix_memory_transparency_edit_conflict_total "
       << edit_conflict_.load(std::memory_order_relaxed) << "\n";

    // cortrix_memory_transparency_invalid_input_total
    os << "# HELP cortrix_memory_transparency_invalid_input_total Invalid-input rejections by error code.\n";
    os << "# TYPE cortrix_memory_transparency_invalid_input_total counter\n";
    for (int e = 0; e < kErrorCodeCount; ++e) {
        os << "cortrix_memory_transparency_invalid_input_total{error_code=\""
           << ToString(static_cast<ErrorCodeLabel>(e)) << "\"} "
           << invalid_input_[e].load(std::memory_order_relaxed) << "\n";
    }

    return os.str();
}

void MemoryMetrics::ResetForTest() {
    for (auto& row : op_total_)
        for (auto& a : row) a.store(0, std::memory_order_relaxed);
    for (int o = 0; o < kOpCount; ++o) {
        latency_sum_ms_[o].store(0, std::memory_order_relaxed);
        latency_count_[o].store(0, std::memory_order_relaxed);
        for (auto& b : latency_bkt_[o]) b.store(0, std::memory_order_relaxed);
    }
    cross_user_blocked_.store(0, std::memory_order_relaxed);
    edit_conflict_.store(0, std::memory_order_relaxed);
    for (auto& a : invalid_input_) a.store(0, std::memory_order_relaxed);
}

const char* ToString(MemoryMetrics::Op op) {
    switch (op) {
        case MemoryMetrics::Op::kList:       return "list";
        case MemoryMetrics::Op::kCreate:     return "create";
        case MemoryMetrics::Op::kEdit:       return "edit";
        case MemoryMetrics::Op::kInvalidate: return "invalidate";
    }
    return "unknown";
}

const char* ToString(MemoryMetrics::OpStatus status) {
    switch (status) {
        case MemoryMetrics::OpStatus::kSuccess: return "success";
        case MemoryMetrics::OpStatus::kError:   return "error";
    }
    return "unknown";
}

const char* ToString(MemoryMetrics::ErrorCodeLabel error_code) {
    switch (error_code) {
        case MemoryMetrics::ErrorCodeLabel::kMemoryNotFound:     return "memory_not_found";
        case MemoryMetrics::ErrorCodeLabel::kUserMismatch:       return "user_mismatch";
        case MemoryMetrics::ErrorCodeLabel::kAlreadyInvalidated: return "already_invalidated";
        case MemoryMetrics::ErrorCodeLabel::kInvalidateFailed:   return "invalidate_failed";
        case MemoryMetrics::ErrorCodeLabel::kQuota:              return "quota";
    }
    return "unknown";
}

}  // namespace cortrix::memory::transparency
