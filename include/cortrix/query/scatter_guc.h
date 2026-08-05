#pragma once
#include <array>
#include <string>

#include "cortrix/common/i_global_config.h"
#include "cortrix/query/scatter_gather.h"  // ScatterTimeouts

namespace cortrix::query {

/// The 5 global GUCs, as a compile-time SoT (name / default / min / max).
///
/// 🚨 D3 standalone (S4.2): this is the **GUC table + range-validated load** from an
/// IGlobalConfig. The real PostgreSQL `DefineCustomVariable` registration (GUC
/// context, flags, hooks) is **D3.5** — it consumes exactly this table so the
/// name/default/range stay single-sourced. `executor.workers` / `executor.queue_size`
/// size the F-Common ExecutorEngine (topic 1.3); the three `scatter.*` knobs drive
/// ScatterGather's per-query cap + dual-layer timeouts (topic 1.5 / 2.5).
struct ScatterGucDef {
    const char* name;     ///< GUC key (e.g. "scatter.ns_query_timeout_ms")
    int default_value;
    int min_value;        ///< §2.7 range lower bound (inclusive)
    int max_value;        ///< §2.7 range upper bound (inclusive)
};

/// The §2.7 GUC clinic. Order is the §2.7 table order; indices below alias into it.
inline constexpr std::array<ScatterGucDef, 5> kScatterGucs = {{
    {"executor.workers", 8, 4, 32},
    {"executor.queue_size", 500, 100, 5000},
    {"scatter.max_namespaces_per_query", 100, 1, 1000},
    {"scatter.ns_query_timeout_ms", 5000, 1000, 60000},
    {"scatter.total_timeout_ms", 30000, 5000, 300000},
}};

enum ScatterGucIndex {
    kGucExecutorWorkers = 0,
    kGucExecutorQueueSize = 1,
    kGucMaxNamespacesPerQuery = 2,
    kGucNsQueryTimeoutMs = 3,
    kGucTotalTimeoutMs = 4,
};

/// Resolved cross-NS runtime config. The values ScatterGather + ExecutorEngine
/// actually run with, after load + range clamp.
struct ScatterConfig {
    int executor_workers = kScatterGucs[kGucExecutorWorkers].default_value;
    int executor_queue_size = kScatterGucs[kGucExecutorQueueSize].default_value;
    int max_namespaces_per_query =
        kScatterGucs[kGucMaxNamespacesPerQuery].default_value;
    int ns_query_timeout_ms = kScatterGucs[kGucNsQueryTimeoutMs].default_value;
    int total_timeout_ms = kScatterGucs[kGucTotalTimeoutMs].default_value;

    /// The dual-layer timeouts (topic 2.5) as a ScatterTimeouts for SetTimeouts().
    ScatterTimeouts Timeouts() const {
        ScatterTimeouts t;
        t.ns_query_timeout_ms = ns_query_timeout_ms;
        t.total_timeout_ms = total_timeout_ms;
        return t;
    }
};

/// Clamp `value` into the GUC's [min, max] (§2.7 range validation). Out-of-range values are
/// clamped (not rejected) — a GUC must always resolve to a usable value; the real
/// DefineCustomVariable hook will additionally WARN on clamp (D3.5).
int ClampScatterGuc(ScatterGucIndex idx, int value);

/// Load the 5 GUCs from `cfg` into a ScatterConfig, clamping each to its §2.7 range.
/// A missing/malformed key falls back to the spec default (also in range). `cfg` may
/// be null (everything → defaults), keeping ScatterGather standalone-constructible.
ScatterConfig LoadScatterConfig(const IGlobalConfig* cfg);

}  // namespace cortrix::query
