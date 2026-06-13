#include "cortrix/query/scatter_guc.h"

namespace cortrix::query {

int ClampScatterGuc(ScatterGucIndex idx, int value) {
    const ScatterGucDef& def = kScatterGucs[static_cast<std::size_t>(idx)];
    if (value < def.min_value) return def.min_value;
    if (value > def.max_value) return def.max_value;
    return value;
}

namespace {

// One GUC's resolved value: cfg->GetInt(name) when present + valid, else the spec
// default; then range-clamped. A null cfg → default. Defaults are themselves in
// range so the result is always usable (§2.7 range validation).
int ResolveGuc(const IGlobalConfig* cfg, ScatterGucIndex idx) {
    const ScatterGucDef& def = kScatterGucs[static_cast<std::size_t>(idx)];
    int value = def.default_value;
    if (cfg != nullptr) {
        Result<int> r = cfg->GetInt(def.name);
        if (r.ok()) value = r.value();
        // NotFound / malformed → keep the default (already in range).
    }
    return ClampScatterGuc(idx, value);
}

}  // namespace

ScatterConfig LoadScatterConfig(const IGlobalConfig* cfg) {
    ScatterConfig c;
    c.executor_workers = ResolveGuc(cfg, kGucExecutorWorkers);
    c.executor_queue_size = ResolveGuc(cfg, kGucExecutorQueueSize);
    c.max_namespaces_per_query = ResolveGuc(cfg, kGucMaxNamespacesPerQuery);
    c.ns_query_timeout_ms = ResolveGuc(cfg, kGucNsQueryTimeoutMs);
    c.total_timeout_ms = ResolveGuc(cfg, kGucTotalTimeoutMs);
    return c;
}

}  // namespace cortrix::query
