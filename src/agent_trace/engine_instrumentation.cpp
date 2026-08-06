#include "cortrix/agent_trace/engine_instrumentation.h"

#include <utility>

#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/mcp_session_handler.h"  // Truncate* / FormatError helpers
#include "cortrix/observability/observability_context.h"

namespace cortrix::agent_trace {

using observability::ObservabilityContext;

EngineInstrumentation::EngineInstrumentation(
    std::shared_ptr<IAgentTraceWriter> trace_writer,
    std::shared_ptr<observability::IOperationLogger> op_logger)
    : trace_writer_(std::move(trace_writer)), op_logger_(std::move(op_logger)) {}

void EngineInstrumentation::Record(const EngineCall& call) {
    // C1: read identity from the thread-local ObservabilityContext.
    const ObservabilityContext& octx = ObservabilityContext::ThreadLocal();
    auto& metrics = AgentTraceMetrics::Instance();

    // 1. agent_trace FIRST (all calls, incl. failures), exception-isolated (C4).
    {
        AgentTraceEntry e;
        e.session_id = octx.session_id;
        e.agent_id = octx.agent_id;
        e.namespace_id = call.namespace_id;
        if (const auto* tc = octx.GetTraceContext(); tc != nullptr && !tc->trace_id.empty()) {
            e.trace_id = tc->trace_id;
        }
        e.method = call.method;
        e.source = call.source;
        e.duration_ms = call.duration_ms;
        if (call.params_json) e.params = McpSessionHandler::TruncateParams(*call.params_json);
        if (call.is_success) {
            e.status = "success";
            if (call.result_summary)
                e.result_summary = McpSessionHandler::TruncateResult(*call.result_summary);
        } else {
            e.status = "failed";
            e.error_code = call.error_code;
            e.result_summary = McpSessionHandler::FormatError(call.error_code, call.error_message);
        }
        try {
            if (trace_writer_) trace_writer_->Write(e);
        } catch (...) {
            // C4: an observability fault must not break the business path.
            metrics.RecordWriteFailed(AgentTraceMetrics::Module::kAgentTrace);
        }
    }

    // 2. operation_log SECOND, only on success, isolated (topic 8). Skipped when
    //    no logger is supplied.
    if (op_logger_ && call.is_success) {
        observability::OperationLogEntry op;
        op.timestamp = 0;  // logger fills now()
        op.user_id = octx.user_id.value_or("anonymous");
        op.action = call.method;
        op.namespace_id = call.namespace_id;
        op.resource_type = call.resource_type;
        op.resource_id = call.resource_id;
        op.summary = call.op_summary;
        if (const auto* tc = octx.GetTraceContext(); tc != nullptr && !tc->trace_id.empty()) {
            op.trace_id = tc->trace_id;
        }
        op.session_id = octx.session_id;
        try {
            op_logger_->Log(op);
        } catch (...) {
            metrics.RecordWriteFailed(AgentTraceMetrics::Module::kOperationLog);
        }
    }
}

}  // namespace cortrix::agent_trace
