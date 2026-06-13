#include "cortrix/observability/operation_log_emitter.h"

#include <utility>

namespace cortrix::observability {

namespace {
constexpr size_t kSummaryMaxBytes = 100;  // §5.1 — summary ≤ 100 chars
}  // namespace

const char* ResourceTypeFor(EmitSite site, const std::string& action) {
    switch (site) {
        case EmitSite::kQueryEngine:
            return "query";
        case EmitSite::kSpcPipeline:
            // §9.1: SpcPipeline emits upload / delete (document) + db_import.
            // §5.1 resource_type domain also has db_connection for the F16a hooks.
            if (action == "database_import") return "db_import";
            if (action == "db_connection_register" || action == "db_connection_revoke")
                return "db_connection";
            return "document";  // upload / delete
        case EmitSite::kMemoryStore:
            return "memory";
        case EmitSite::kNamespaceManager:
            return "namespace";
    }
    return "query";  // unreachable for a valid enum
}

std::optional<std::string> TruncateSummary(const std::optional<std::string>& summary) {
    if (!summary.has_value()) return std::nullopt;
    if (summary->size() <= kSummaryMaxBytes) return summary;
    return summary->substr(0, kSummaryMaxBytes);
}

OperationLogEntry MakeEngineEntry(EmitSite site,
                                  const std::string& action,
                                  std::optional<std::string> namespace_id,
                                  std::optional<std::string> resource_id,
                                  std::optional<std::string> summary) {
    OperationLogEntry e;
    e.timestamp = 0;  // logger fills now()
    e.action = action;
    e.resource_type = ResourceTypeFor(site, action);
    e.namespace_id = std::move(namespace_id);
    e.resource_id = std::move(resource_id);
    e.summary = TruncateSummary(summary);

    // C1: read trace_id / session_id / user_id from the thread-local context.
    const ObservabilityContext& octx = ObservabilityContext::ThreadLocal();
    if (const TraceContext* tc = octx.GetTraceContext(); tc != nullptr) {
        if (!tc->trace_id.empty()) e.trace_id = tc->trace_id;
        // span_id is not the session_id; session correlation (C2) is supplied by
        // the caller's context when the full ObservabilityContext carries it.
        // Phase 1: trace_id from the W3C TraceContext; session_id stays unset here
        // unless the caller sets it explicitly on the returned entry.
    }
    // user_id: §9.2 default "anonymous" until Auth (P08) populates a real id on
    // the context. The Phase-1 ObservabilityContext does not yet carry user_id;
    // the instrumentation site sets it from its request context (D3.5 wiring). Default here.
    e.user_id = "anonymous";
    return e;
}

}  // namespace cortrix::observability
