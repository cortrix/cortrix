#pragma once
#include <memory>
#include <optional>
#include <string>

#include "cortrix/observability/observability_context.h"
#include "cortrix/observability/operation_logger.h"

namespace cortrix::observability {

/// The 4 Engine instrumentation sites. Each Engine module emits operation_log
/// entries for a fixed set of actions; this enum names the site so the emitter
/// helper can fill the correct resource_type and the standalone tests can assert
/// the mapping without touching the real Engine code (standalone — the
/// actual Engine wiring is integration).
enum class EmitSite {
    kQueryEngine,       // query / cross_ns_query        → resource_type "query"
    kSpcPipeline,       // upload / delete / db_import    → document / db_import
    kMemoryStore,       // memory_*                       → memory
    kNamespaceManager,  // ns_*                           → namespace
};

/// Mixin every instrumentation-bearing Engine module implements (QueryEngine / SpcPipeline /
/// MemoryStore / NamespaceManager) so the DI container can inject the logger
/// uniformly. Default-null so a module built before the logger exists is a
/// no-op (Log is simply skipped) — keeps observability strictly additive to the
/// business path (C4). The real `implements` lands at integration.
class IOperationLoggerAware {
public:
    virtual ~IOperationLoggerAware() = default;
    virtual void SetOperationLogger(std::shared_ptr<IOperationLogger> logger) = 0;
};

/// Build the operation_log entry for an Engine instrumentation site. Centralizes the
/// site → resource_type mapping + the summary truncation (≤100 chars) +
/// the C1 ObservabilityContext read (trace_id / session_id / user_id from the
/// thread-local), so all 4 instrumentation sites stay consistent and the contract is unit
/// tested now. `user_id` falls back to "anonymous". Only call on the
/// success path (operation_log records successful operations only).
OperationLogEntry MakeEngineEntry(EmitSite site,
                                  const std::string& action,
                                  std::optional<std::string> namespace_id,
                                  std::optional<std::string> resource_id,
                                  std::optional<std::string> summary);

/// resource_type string for an EmitSite + action. SpcPipeline
/// distinguishes document vs db_import by the action (`database_import` →
/// "db_import", else "document"). db_connection_* actions → "db_connection".
const char* ResourceTypeFor(EmitSite site, const std::string& action);

/// Truncate a summary to the ≤100-char cap (UTF-8-naive byte truncation,
/// matching the spec's [:100] semantics). nullopt passes through.
std::optional<std::string> TruncateSummary(const std::optional<std::string>& summary);

}  // namespace cortrix::observability
