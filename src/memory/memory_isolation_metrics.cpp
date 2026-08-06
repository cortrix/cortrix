#include <cstdint>
#include "cortrix/memory/memory_isolation_metrics.h"

#include <cstring>
#include <sstream>

namespace cortrix::memory {

namespace {

// A gauge holding a double via its IEEE-754 bit pattern in an atomic<uint64_t>
// (lock-free store/load of a floating-point value; mirrors how a Prometheus gauge
// just overwrites its last value).
void StoreDouble(std::atomic<uint64_t>& slot, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    slot.store(bits, std::memory_order_relaxed);
}

double LoadDouble(const std::atomic<uint64_t>& slot) {
    uint64_t bits = slot.load(std::memory_order_relaxed);
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

}  // namespace

MemoryIsolationMetrics& MemoryIsolationMetrics::Instance() {
    static MemoryIsolationMetrics instance;
    return instance;
}

void MemoryIsolationMetrics::RecordIsolationCheck(CheckResult result, Action action) {
    isolation_check_[static_cast<int>(result)][static_cast<int>(action)].fetch_add(
        1, std::memory_order_relaxed);
}

uint64_t MemoryIsolationMetrics::IsolationCheckCount(CheckResult result, Action action) const {
    return isolation_check_[static_cast<int>(result)][static_cast<int>(action)].load(
        std::memory_order_relaxed);
}

void MemoryIsolationMetrics::RecordIsolationViolation(Action action, Reason reason) {
    isolation_violation_[static_cast<int>(action)][static_cast<int>(reason)].fetch_add(
        1, std::memory_order_relaxed);
}

uint64_t MemoryIsolationMetrics::IsolationViolationCount(Action action, Reason reason) const {
    return isolation_violation_[static_cast<int>(action)][static_cast<int>(reason)].load(
        std::memory_order_relaxed);
}

void MemoryIsolationMetrics::RecordQuotaExceeded(QuotaType quota_type) {
    quota_exceeded_[static_cast<int>(quota_type)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryIsolationMetrics::QuotaExceededCount(QuotaType quota_type) const {
    return quota_exceeded_[static_cast<int>(quota_type)].load(std::memory_order_relaxed);
}

void MemoryIsolationMetrics::SetQuotaUsageRatio(QuotaType quota_type, double ratio) {
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;  // §8.bis: ratio is 0-1.
    StoreDouble(quota_usage_ratio_bits_[static_cast<int>(quota_type)], ratio);
}

double MemoryIsolationMetrics::QuotaUsageRatio(QuotaType quota_type) const {
    return LoadDouble(quota_usage_ratio_bits_[static_cast<int>(quota_type)]);
}

void MemoryIsolationMetrics::SetUserSessionCount(int64_t count) {
    if (count < 0) count = 0;
    user_session_count_.store(count, std::memory_order_relaxed);
}

int64_t MemoryIsolationMetrics::UserSessionCount() const {
    return user_session_count_.load(std::memory_order_relaxed);
}

void MemoryIsolationMetrics::RecordDefaultUserUsed() {
    default_user_used_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryIsolationMetrics::DefaultUserUsedCount() const {
    return default_user_used_.load(std::memory_order_relaxed);
}

void MemoryIsolationMetrics::RecordMatchScopeExcluded(Reason reason) {
    match_scope_excluded_[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryIsolationMetrics::MatchScopeExcludedCount(Reason reason) const {
    return match_scope_excluded_[static_cast<int>(reason)].load(std::memory_order_relaxed);
}

std::string MemoryIsolationMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_memory_isolation_check_total (counter, labels: result, action)
    os << "# HELP cortrix_memory_isolation_check_total Per-user isolation checks by result and action.\n";
    os << "# TYPE cortrix_memory_isolation_check_total counter\n";
    for (int r = 0; r < kCheckResultCount; ++r) {
        for (int a = 0; a < kActionCount; ++a) {
            os << "cortrix_memory_isolation_check_total{result=\""
               << ToString(static_cast<CheckResult>(r)) << "\",action=\""
               << ToString(static_cast<Action>(a)) << "\"} "
               << isolation_check_[r][a].load(std::memory_order_relaxed) << "\n";
        }
    }

    // cortrix_memory_isolation_violation_total (counter, labels: action, reason)
    os << "# HELP cortrix_memory_isolation_violation_total Cross-user access denials (safety alert).\n";
    os << "# TYPE cortrix_memory_isolation_violation_total counter\n";
    for (int a = 0; a < kActionCount; ++a) {
        for (int rn = 0; rn < kReasonCount; ++rn) {
            os << "cortrix_memory_isolation_violation_total{action=\""
               << ToString(static_cast<Action>(a)) << "\",reason=\""
               << ToString(static_cast<Reason>(rn)) << "\"} "
               << isolation_violation_[a][rn].load(std::memory_order_relaxed) << "\n";
        }
    }

    // cortrix_memory_isolation_quota_exceeded_total (counter, label: quota_type)
    os << "# HELP cortrix_memory_isolation_quota_exceeded_total User quota-exceeded events by quota_type.\n";
    os << "# TYPE cortrix_memory_isolation_quota_exceeded_total counter\n";
    for (int q = 0; q < kQuotaTypeCount; ++q) {
        os << "cortrix_memory_isolation_quota_exceeded_total{quota_type=\""
           << ToString(static_cast<QuotaType>(q)) << "\"} "
           << quota_exceeded_[q].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_memory_isolation_quota_usage_ratio (gauge, label: quota_type)
    os << "# HELP cortrix_memory_isolation_quota_usage_ratio Current quota usage ratio (0-1) by quota_type.\n";
    os << "# TYPE cortrix_memory_isolation_quota_usage_ratio gauge\n";
    for (int q = 0; q < kQuotaTypeCount; ++q) {
        os << "cortrix_memory_isolation_quota_usage_ratio{quota_type=\""
           << ToString(static_cast<QuotaType>(q)) << "\"} "
           << LoadDouble(quota_usage_ratio_bits_[q]) << "\n";
    }

    // cortrix_memory_isolation_user_session_count (gauge, no label)
    os << "# HELP cortrix_memory_isolation_user_session_count Active user-session total.\n";
    os << "# TYPE cortrix_memory_isolation_user_session_count gauge\n";
    os << "cortrix_memory_isolation_user_session_count "
       << user_session_count_.load(std::memory_order_relaxed) << "\n";

    // cortrix_memory_isolation_default_user_used_total (counter, no label)
    os << "# HELP cortrix_memory_isolation_default_user_used_total CE no-auth default user usage count.\n";
    os << "# TYPE cortrix_memory_isolation_default_user_used_total counter\n";
    os << "cortrix_memory_isolation_default_user_used_total "
       << default_user_used_.load(std::memory_order_relaxed) << "\n";

    // cortrix_memory_isolation_match_scope_excluded_total (counter, label: reason)
    os << "# HELP cortrix_memory_isolation_match_scope_excluded_total MatchScope pre-filter exclusions by reason.\n";
    os << "# TYPE cortrix_memory_isolation_match_scope_excluded_total counter\n";
    for (int rn = 0; rn < kReasonCount; ++rn) {
        os << "cortrix_memory_isolation_match_scope_excluded_total{reason=\""
           << ToString(static_cast<Reason>(rn)) << "\"} "
           << match_scope_excluded_[rn].load(std::memory_order_relaxed) << "\n";
    }

    return os.str();
}

void MemoryIsolationMetrics::ResetForTest() {
    for (auto& row : isolation_check_)
        for (auto& a : row) a.store(0, std::memory_order_relaxed);
    for (auto& row : isolation_violation_)
        for (auto& a : row) a.store(0, std::memory_order_relaxed);
    for (auto& a : quota_exceeded_) a.store(0, std::memory_order_relaxed);
    for (auto& a : quota_usage_ratio_bits_) StoreDouble(a, 0.0);
    user_session_count_.store(0, std::memory_order_relaxed);
    default_user_used_.store(0, std::memory_order_relaxed);
    for (auto& a : match_scope_excluded_) a.store(0, std::memory_order_relaxed);
}

const char* ToString(MemoryIsolationMetrics::CheckResult result) {
    switch (result) {
        case MemoryIsolationMetrics::CheckResult::kPass:      return "pass";
        case MemoryIsolationMetrics::CheckResult::kViolation: return "violation";
    }
    return "unknown";
}

const char* ToString(MemoryIsolationMetrics::Action action) {
    switch (action) {
        case MemoryIsolationMetrics::Action::kSearch:        return "search";
        case MemoryIsolationMetrics::Action::kList:          return "list";
        case MemoryIsolationMetrics::Action::kEdit:          return "edit";
        case MemoryIsolationMetrics::Action::kDelete:        return "delete";
        case MemoryIsolationMetrics::Action::kSession:       return "session";
        case MemoryIsolationMetrics::Action::kSessionAccess: return "session_access";
    }
    return "unknown";
}

const char* ToString(MemoryIsolationMetrics::Reason reason) {
    switch (reason) {
        case MemoryIsolationMetrics::Reason::kMissingUserId: return "missing_user_id";
        case MemoryIsolationMetrics::Reason::kEmptyUserId:   return "empty_user_id";
        case MemoryIsolationMetrics::Reason::kMismatch:      return "mismatch";
    }
    return "unknown";
}

const char* ToString(MemoryIsolationMetrics::QuotaType quota_type) {
    switch (quota_type) {
        case MemoryIsolationMetrics::QuotaType::kItemsCount:   return "items_count";
        case MemoryIsolationMetrics::QuotaType::kSessionCount: return "session_count";
        case MemoryIsolationMetrics::QuotaType::kMemoryBytes:  return "memory_bytes";
    }
    return "unknown";
}

}  // namespace cortrix::memory
