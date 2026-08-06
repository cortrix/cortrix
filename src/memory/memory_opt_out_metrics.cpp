#include <cstdint>
#include "cortrix/memory/memory_opt_out_metrics.h"

#include <sstream>

namespace cortrix::memory::immunity {

MemoryOptOutMetrics& MemoryOptOutMetrics::Instance() {
    static MemoryOptOutMetrics instance;
    return instance;
}

void MemoryOptOutMetrics::RecordOptOut(TriggeredBy triggered_by) {
    opt_out_[static_cast<int>(triggered_by)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryOptOutMetrics::OptOutCount(TriggeredBy triggered_by) const {
    return opt_out_[static_cast<int>(triggered_by)].load(std::memory_order_relaxed);
}

void MemoryOptOutMetrics::RecordOptOutRevoke() {
    opt_out_revoke_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryOptOutMetrics::OptOutRevokeCount() const {
    return opt_out_revoke_.load(std::memory_order_relaxed);
}

void MemoryOptOutMetrics::RecordExtractSkipped() {
    extract_skipped_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryOptOutMetrics::ExtractSkippedCount() const {
    return extract_skipped_.load(std::memory_order_relaxed);
}

std::string MemoryOptOutMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_memory_opt_out_total
    os << "# HELP cortrix_memory_opt_out_total MEM04 session opt-out events by trigger.\n";
    os << "# TYPE cortrix_memory_opt_out_total counter\n";
    for (int t = 0; t < kTriggeredByCount; ++t) {
        os << "cortrix_memory_opt_out_total{triggered_by=\""
           << ToString(static_cast<TriggeredBy>(t)) << "\"} "
           << opt_out_[t].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_memory_opt_out_revoke_total
    os << "# HELP cortrix_memory_opt_out_revoke_total MEM04 session opt-out revocations.\n";
    os << "# TYPE cortrix_memory_opt_out_revoke_total counter\n";
    os << "cortrix_memory_opt_out_revoke_total "
       << opt_out_revoke_.load(std::memory_order_relaxed) << "\n";

    // cortrix_memory_opt_out_extract_skipped_total
    os << "# HELP cortrix_memory_opt_out_extract_skipped_total MEM02 extractions skipped for opted-out sessions.\n";
    os << "# TYPE cortrix_memory_opt_out_extract_skipped_total counter\n";
    os << "cortrix_memory_opt_out_extract_skipped_total "
       << extract_skipped_.load(std::memory_order_relaxed) << "\n";

    return os.str();
}

void MemoryOptOutMetrics::ResetForTest() {
    for (auto& a : opt_out_) a.store(0, std::memory_order_relaxed);
    opt_out_revoke_.store(0, std::memory_order_relaxed);
    extract_skipped_.store(0, std::memory_order_relaxed);
}

const char* ToString(MemoryOptOutMetrics::TriggeredBy triggered_by) {
    switch (triggered_by) {
        case MemoryOptOutMetrics::TriggeredBy::kUser:   return "user";
        case MemoryOptOutMetrics::TriggeredBy::kAgent:  return "agent";
        case MemoryOptOutMetrics::TriggeredBy::kSystem: return "system";
    }
    return "unknown";
}

}  // namespace cortrix::memory::immunity
