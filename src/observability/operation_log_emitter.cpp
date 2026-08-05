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
            // The resource_type domain also has db_connection for the DB-import hooks.
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

    // C1/C2: read trace_id / session_id / user_id from the thread-local context.
    // The entry point (WithAuth, auth_middleware.cpp) now fills the ObservabilityContext
    // from the identity headers + the authenticated principal before the handler
    // runs, so this fulfills the previously-deferred D3.5 wiring: trace_id comes from the
    // W3C TraceContext; session_id (C2) + user_id come from the identity fields.
    const ObservabilityContext& octx = ObservabilityContext::ThreadLocal();
    if (const TraceContext* tc = octx.GetTraceContext(); tc != nullptr) {
        if (!tc->trace_id.empty()) e.trace_id = tc->trace_id;
    }
    // C2 session correlation — set when the entry point populated it (NULL allowed).
    if (octx.session_id.has_value() && !octx.session_id->empty()) {
        e.session_id = octx.session_id;
    }
    // user_id: from the authenticated principal the entry point installed; §9.2
    // default "anonymous" when the context carries none (dev / no-auth path).
    e.user_id = (octx.user_id.has_value() && !octx.user_id->empty())
                    ? *octx.user_id
                    : "anonymous";
    return e;
}

}  // namespace cortrix::observability
