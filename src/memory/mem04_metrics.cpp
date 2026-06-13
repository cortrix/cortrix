#include "cortrix/memory/mem04_metrics.h"

#include <sstream>

namespace cortrix::memory::immunity {

Mem04Metrics& Mem04Metrics::Instance() {
    static Mem04Metrics instance;
    return instance;
}

void Mem04Metrics::RecordOptOut(TriggeredBy triggered_by) {
    opt_out_[static_cast<int>(triggered_by)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t Mem04Metrics::OptOutCount(TriggeredBy triggered_by) const {
    return opt_out_[static_cast<int>(triggered_by)].load(std::memory_order_relaxed);
}

void Mem04Metrics::RecordOptOutRevoke() {
    opt_out_revoke_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Mem04Metrics::OptOutRevokeCount() const {
    return opt_out_revoke_.load(std::memory_order_relaxed);
}

void Mem04Metrics::RecordExtractSkipped() {
    extract_skipped_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Mem04Metrics::ExtractSkippedCount() const {
    return extract_skipped_.load(std::memory_order_relaxed);
}

std::string Mem04Metrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_mem04_opt_out_total
    os << "# HELP cortrix_mem04_opt_out_total MEM04 session opt-out events by trigger.\n";
    os << "# TYPE cortrix_mem04_opt_out_total counter\n";
    for (int t = 0; t < kTriggeredByCount; ++t) {
        os << "cortrix_mem04_opt_out_total{triggered_by=\""
           << ToString(static_cast<TriggeredBy>(t)) << "\"} "
           << opt_out_[t].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_mem04_opt_out_revoke_total
    os << "# HELP cortrix_mem04_opt_out_revoke_total MEM04 session opt-out revocations.\n";
    os << "# TYPE cortrix_mem04_opt_out_revoke_total counter\n";
    os << "cortrix_mem04_opt_out_revoke_total "
       << opt_out_revoke_.load(std::memory_order_relaxed) << "\n";

    // cortrix_mem04_extract_skipped_total
    os << "# HELP cortrix_mem04_extract_skipped_total MEM02 extractions skipped for opted-out sessions.\n";
    os << "# TYPE cortrix_mem04_extract_skipped_total counter\n";
    os << "cortrix_mem04_extract_skipped_total "
       << extract_skipped_.load(std::memory_order_relaxed) << "\n";

    return os.str();
}

void Mem04Metrics::ResetForTest() {
    for (auto& a : opt_out_) a.store(0, std::memory_order_relaxed);
    opt_out_revoke_.store(0, std::memory_order_relaxed);
    extract_skipped_.store(0, std::memory_order_relaxed);
}

const char* ToString(Mem04Metrics::TriggeredBy triggered_by) {
    switch (triggered_by) {
        case Mem04Metrics::TriggeredBy::kUser:   return "user";
        case Mem04Metrics::TriggeredBy::kAgent:  return "agent";
        case Mem04Metrics::TriggeredBy::kSystem: return "system";
    }
    return "unknown";
}

}  // namespace cortrix::memory::immunity
