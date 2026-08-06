#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::memory::immunity {

/// The `mem04` subsystem metrics (observability naming,
/// naming `cortrix_memory_opt_out_<event>_<unit>`). Mirrors the memory_metrics.h /
/// memory_extract_metrics.h template (process-wide singleton, atomic counters, OpenMetrics
/// renderer).
///
/// 🚨 Subsystem name: the metric prefix is `cortrix_memory_opt_out_*` (a "gray" subsystem in
/// OBS_SPEC §2.3 / ARCH §1.7.1). The rename to `memory_immunity` is the Phase-2
/// TD-OBS-SUBSYSTEM-RENAME backlog item — NOT done here.
///
/// 🚨 Cardinality control (OBSERVABILITY_SPEC §3.2): labels are enum-only.
/// `opt_out_total` carries the `triggered_by` enum (user / agent / system); the
/// `ns` label was removed from `extract_skipped_total` (v1.0.x D1 V3 decision 10 —
/// `ns` is high-cardinality, forbidden). Per-NS data goes through the §3.4 per-tenant
/// API, NOT this metric.
///
/// 🚨 D3 standalone: a self-contained, dependency-free recorder + an OpenMetrics text
/// renderer. The `/metrics` scrape endpoint does not exist in the frozen tree —
/// registering this recorder into that endpoint is cross-Feature wiring **deferred to
/// D3.5**. Until then it is fully usable + testable in-process and RenderOpenMetrics()
/// produces what the server will serve.
///
/// §5.5 metric schema (3 rows, all counters):
///   cortrix_memory_opt_out_total          counter {triggered_by}
///   cortrix_memory_opt_out_revoke_total   counter {}
///   cortrix_memory_opt_out_extract_skipped_total  counter {}  (no label — V3 decision 10)
class MemoryOptOutMetrics {
public:
    /// triggered_by label for opt_out_total (§5.5). Mirrors the `opted_out_by`
    /// request enum collapsed to its low-cardinality actor class: user_manual → user,
    /// agent_auto → agent, system_auto / test → system. Keeping it a 3-value enum
    /// (not the raw 4-value opted_out_by) bounds the label set.
    enum class TriggeredBy {
        kUser = 0,
        kAgent,
        kSystem,
    };

    /// Process-wide instance (metrics are global counters).
    static MemoryOptOutMetrics& Instance();

    // --- cortrix_memory_opt_out_total (Counter, label: triggered_by) ---
    void RecordOptOut(TriggeredBy triggered_by);
    uint64_t OptOutCount(TriggeredBy triggered_by) const;

    // --- cortrix_memory_opt_out_revoke_total (Counter) ---
    void RecordOptOutRevoke();
    uint64_t OptOutRevokeCount() const;

    // --- cortrix_memory_opt_out_extract_skipped_total (Counter) ---
    // Incremented when the extraction worker skips an opted-out session.
    void RecordExtractSkipped();
    uint64_t ExtractSkippedCount() const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    MemoryOptOutMetrics() = default;

    static constexpr int kTriggeredByCount = 3;  // user / agent / system

    std::array<std::atomic<uint64_t>, kTriggeredByCount> opt_out_{};
    std::atomic<uint64_t> opt_out_revoke_{0};
    std::atomic<uint64_t> extract_skipped_{0};
};

const char* ToString(MemoryOptOutMetrics::TriggeredBy triggered_by);

}  // namespace cortrix::memory::immunity
