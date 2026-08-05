#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::memory {

/// The `mem05` subsystem metrics (MEM05 §8.bis, OBSERVABILITY_SPEC §2.3 naming
/// `cortrix_mem05_<metric>_<unit>`). Self-contained dependency-free recorder
/// (same pattern as Mem02Metrics / ScoringMetrics): a process-wide singleton of
/// atomic counters/gauges + an OpenMetrics text renderer.
///
/// MEM05 metrics focus on *isolation compliance* — per-user isolation checks,
/// cross-user access denials (the safety-critical alert), quota enforcement, the
/// CE no-auth `default` user, and MatchScope pre-filtering. They deliberately do
/// NOT re-collect the generic `memory_*` metrics (items_count / decay_ratio,
/// owned by MEM01 / MEM03), per MEM05 §8.bis.
///
/// 🚨 Cardinality control (OBSERVABILITY_SPEC §3.2 — MEM05 §8.bis): labels are
/// enum-only (result / action / reason / quota_type). `user_id` is on the §3.2
/// absolute deny list (high cardinality); per-user data goes through the audit
/// log, never a metric label.
///
/// 🚨 D3 standalone: this recorder + RenderOpenMetrics() are fully usable +
/// testable in-process. The F24 `/metrics` scrape endpoint does not exist in the
/// frozen tree — registering this recorder into that endpoint is cross-Feature
/// wiring **deferred to D3.5** (same status as ScoringMetrics / Mem02Metrics).
///
/// §8.bis metric schema (7 rows):
///   cortrix_mem05_isolation_check_total       counter  {result, action}
///   cortrix_mem05_isolation_violation_total   counter  {action, reason}  (safety alert)
///   cortrix_mem05_quota_exceeded_total        counter  {quota_type}
///   cortrix_mem05_quota_usage_ratio           gauge    {quota_type}
///   cortrix_mem05_user_session_count          gauge    (no label)
///   cortrix_mem05_default_user_used_total     counter  (no label — CE compat)
///   cortrix_mem05_match_scope_excluded_total  counter  {reason}
class Mem05Metrics {
public:
    /// `result` label for isolation_check_total (§8.bis).
    enum class CheckResult {
        kPass = 0,
        kViolation,
    };

    /// `action` label for isolation_check_total / isolation_violation_total
    /// (§8.bis). `session` (violation) and `session_access` (check) keep the spec's
    /// distinct wording — both are valid label values, mapped by the action enum.
    enum class Action {
        kSearch = 0,
        kList,
        kEdit,
        kDelete,
        kSession,         ///< isolation_violation_total action value "session"
        kSessionAccess,   ///< isolation_check_total action value "session_access"
    };

    /// `reason` label for isolation_violation_total / match_scope_excluded_total
    /// (§8.bis). The controlled vocabulary for why a user_id check failed.
    enum class Reason {
        kMissingUserId = 0,
        kEmptyUserId,
        kMismatch,
    };

    /// `quota_type` label for quota_exceeded_total / quota_usage_ratio (§8.bis).
    enum class QuotaType {
        kItemsCount = 0,
        kSessionCount,
        kMemoryBytes,
    };

    /// Process-wide instance (metrics are global counters/gauges).
    static Mem05Metrics& Instance();

    // --- cortrix_mem05_isolation_check_total (Counter, labels: result, action) ---
    // The isolation-check audit baseline: every per-user isolation decision point.
    void RecordIsolationCheck(CheckResult result, Action action);
    uint64_t IsolationCheckCount(CheckResult result, Action action) const;

    // --- cortrix_mem05_isolation_violation_total (Counter, labels: action, reason) ---
    // Cross-user access denied — the safety-critical alert metric.
    void RecordIsolationViolation(Action action, Reason reason);
    uint64_t IsolationViolationCount(Action action, Reason reason) const;

    // --- cortrix_mem05_quota_exceeded_total (Counter, label: quota_type) ---
    void RecordQuotaExceeded(QuotaType quota_type);
    uint64_t QuotaExceededCount(QuotaType quota_type) const;

    // --- cortrix_mem05_quota_usage_ratio (Gauge, label: quota_type) ---
    // Current usage ratio in [0,1] per quota_type; near 1.0 → ops-side alert.
    void SetQuotaUsageRatio(QuotaType quota_type, double ratio);
    double QuotaUsageRatio(QuotaType quota_type) const;

    // --- cortrix_mem05_user_session_count (Gauge, no label) ---
    // Active user-session total (no user_id label — §3.2; per-user via audit log).
    void SetUserSessionCount(int64_t count);
    int64_t UserSessionCount() const;

    // --- cortrix_mem05_default_user_used_total (Counter, no label) ---
    // CE no-auth default user="default" usage (topic 8 CE-compat observation).
    void RecordDefaultUserUsed();
    uint64_t DefaultUserUsedCount() const;

    // --- cortrix_mem05_match_scope_excluded_total (Counter, label: reason) ---
    // MatchScope pre-filter exclusions (retrieval-path pre-filter, distinct from
    // the API-entry isolation_violation_total).
    void RecordMatchScopeExcluded(Reason reason);
    uint64_t MatchScopeExcludedCount(Reason reason) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    /// Stable metric names + HELP/TYPE lines (what the F24 endpoint will serve).
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauges (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    Mem05Metrics() = default;

    static constexpr int kCheckResultCount = 2;  // pass / violation
    static constexpr int kActionCount = 6;       // search / list / edit / delete / session / session_access
    static constexpr int kReasonCount = 3;       // missing_user_id / empty_user_id / mismatch
    static constexpr int kQuotaTypeCount = 3;    // items_count / session_count / memory_bytes

    // isolation_check_total[result][action]
    std::array<std::array<std::atomic<uint64_t>, kActionCount>, kCheckResultCount>
        isolation_check_{};
    // isolation_violation_total[action][reason]
    std::array<std::array<std::atomic<uint64_t>, kReasonCount>, kActionCount>
        isolation_violation_{};
    std::array<std::atomic<uint64_t>, kQuotaTypeCount> quota_exceeded_{};
    // quota_usage_ratio is a float gauge; store the IEEE-754 bits in an atomic.
    std::array<std::atomic<uint64_t>, kQuotaTypeCount> quota_usage_ratio_bits_{};
    std::atomic<int64_t> user_session_count_{0};
    std::atomic<uint64_t> default_user_used_{0};
    std::array<std::atomic<uint64_t>, kReasonCount> match_scope_excluded_{};
};

const char* ToString(Mem05Metrics::CheckResult result);
const char* ToString(Mem05Metrics::Action action);
const char* ToString(Mem05Metrics::Reason reason);
const char* ToString(Mem05Metrics::QuotaType quota_type);

}  // namespace cortrix::memory
