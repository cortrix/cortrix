#pragma once
#include <memory>
#include <optional>
#include <string>

#include "cortrix/agent_trace/agent_trace_writer.h"
#include "cortrix/observability/operation_logger.h"

namespace cortrix::agent_trace {

/// The minimal description of one Engine call to be traced (F13 §11, S6). The
/// instrumentation site fills this from its request + result; the helper reads the
/// rest (trace_id/session_id/agent_id/user_id) from the thread-local
/// ObservabilityContext.
struct EngineCall {
    std::string method;                       ///< "query" / "upload" / "memory_search" / ...
    std::string source = "http";              ///< "http" | "mcp" (entry-dependent)
    std::optional<std::string> namespace_id;
    std::optional<std::string> params_json;   ///< raw params (truncated to ≤2KB on write)
    bool is_success = true;
    std::optional<std::string> result_summary;///< on success (truncated to ≤512)
    std::optional<std::string> error_code;    ///< on failure
    std::string error_message;                ///< on failure (first 256 chars kept)
    int duration_ms = 0;

    // F18a operation_log fields (only used when an operation_logger is supplied +
    // is_success). resource_type/resource_id/summary mirror OperationLogEntry.
    std::string resource_type = "query";
    std::optional<std::string> resource_id;
    std::optional<std::string> op_summary;    ///< operation_log summary (≤100)
};

/// Reusable Engine-layer instrumentation (F13 §11, topic 8 C4). Wraps the
/// dual-write both Engine sites (QueryEngine / SpcPipeline) perform after running
/// the business logic:
///   1. F13 agent_trace FIRST, for ALL calls (incl. failures), inside a try/catch
///      so an observability fault never breaks the business path (C4 exception
///      isolation); a thrown write bumps cortrix_observability_write_failed_total{f13}.
///   2. F18a operation_log SECOND, only on success, in its own try/catch
///      (write_failed{f18a} on throw). Skipped when no operation_logger is given.
/// Identity (trace_id/session_id/agent_id/user_id) is read from the thread-local
/// ObservabilityContext (C1). Pure helper — standalone: the real Engine call sites
/// invoke this at D3.5; here it is fully testable with mock writers (incl. ones
/// that throw).
///
/// The agent_trace writer is required; the operation_logger is optional (a query
/// path double-writes, an internal path may trace only).
class EngineInstrumentation {
public:
    explicit EngineInstrumentation(
        std::shared_ptr<IAgentTraceWriter> trace_writer,
        std::shared_ptr<observability::IOperationLogger> op_logger = nullptr);

    /// Record one finished Engine call (reads the thread-local ObservabilityContext
    /// for identity). Never throws — both writes are isolated (C4).
    void Record(const EngineCall& call);

private:
    std::shared_ptr<IAgentTraceWriter> trace_writer_;
    std::shared_ptr<observability::IOperationLogger> op_logger_;
};

}  // namespace cortrix::agent_trace
