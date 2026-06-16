#include <cstdint>
#include "cortrix/agent_trace/mcp_session_handler.h"

#include <chrono>
#include <utility>
#include <vector>

#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/http_observability_middleware.h"  // GenerateUuidV4
#include "cortrix/observability/observability_context.h"

namespace cortrix::agent_trace {

using observability::ObservabilityValidator;

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// §3 truncation budgets.
constexpr size_t kParamsMaxBytes = 2048;          // ≤2KB
constexpr size_t kParamsHeadBytes = 1536;         // first 1.5KB
constexpr size_t kParamsTailBytes = 512;          // last 0.5KB
constexpr const char* kTruncMid = "[...truncated...]";
constexpr size_t kResultMaxBytes = 512;
constexpr size_t kResultHeadBytes = 400;          // mirrors §8.2 snippet 400+100 split
constexpr size_t kResultTailBytes = 100;
constexpr const char* kTruncTail = "[truncated]";
constexpr size_t kErrorMsgMaxBytes = 256;         // first 256 chars of error message

}  // namespace

std::string McpSessionHandler::TruncateParams(const std::string& params) {
    if (params.size() <= kParamsMaxBytes) return params;
    // Keep head + tail with a middle marker (§3 — preserve first 1.5KB + last 0.5KB).
    return params.substr(0, kParamsHeadBytes) + kTruncMid +
           params.substr(params.size() - kParamsTailBytes);
}

std::string McpSessionHandler::TruncateResult(const std::string& summary) {
    if (summary.size() <= kResultMaxBytes) return summary;
    return summary.substr(0, kResultHeadBytes) + kTruncTail +
           summary.substr(summary.size() - kResultTailBytes);
}

std::string McpSessionHandler::FormatError(const std::optional<std::string>& error_code,
                                           const std::string& error_message) {
    // §3: on failure keep only the error_code + first 256 chars of the message.
    std::string code = error_code.value_or("CX_ERR_F13_INTERNAL");
    std::string msg = error_message.size() > kErrorMsgMaxBytes
                          ? error_message.substr(0, kErrorMsgMaxBytes)
                          : error_message;
    return code + ": " + msg;
}

McpSessionHandler::McpSessionHandler(std::shared_ptr<IAgentTraceWriter> writer,
                                     int idle_timeout_seconds)
    : McpSessionHandler(std::move(writer), idle_timeout_seconds,
                        &GenerateUuidV4, &NowMs) {}

McpSessionHandler::McpSessionHandler(std::shared_ptr<IAgentTraceWriter> writer,
                                     int idle_timeout_seconds,
                                     UuidGenerator uuid_gen, ClockFn clock)
    : writer_(std::move(writer)),
      idle_timeout_seconds_(idle_timeout_seconds),
      uuid_gen_(std::move(uuid_gen)),
      clock_(std::move(clock)) {}

std::string McpSessionHandler::OnConnectionEstablished(const McpClientCapability& cap) {
    // topic 5: server generates a session_id unless the client supplied a valid one.
    std::string session_id;
    if (cap.client_session_id.has_value()) {
        auto r = ObservabilityValidator::ValidateSessionId(*cap.client_session_id);
        session_id = r.ok() ? r.value() : uuid_gen_();
    } else {
        session_id = uuid_gen_();
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        SessionState st;
        st.agent_id = cap.agent_id;
        st.namespace_id = cap.namespace_id;
        st.tool_call_count = 0;
        st.last_activity_ms = clock_();
        sessions_[session_id] = std::move(st);
    }
    AgentTraceMetrics::Instance().IncActiveSessions();
    return session_id;
}

bool McpSessionHandler::OnToolCall(const std::string& session_id, const McpToolCall& call,
                                   const McpToolResult& result) {
    std::optional<std::string> agent_id;
    std::optional<std::string> namespace_id;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            // Unknown session (never established / already closed): treat as a lazy
            // register so a stray tool_call is still traced, but it counts from 0.
            SessionState st;
            st.last_activity_ms = clock_();
            it = sessions_.emplace(session_id, std::move(st)).first;
            AgentTraceMetrics::Instance().IncActiveSessions();
        }
        SessionState& st = it->second;

        // Phase-1 hard limit (topic 5): beyond 10K tool_calls, drop new traces.
        if (st.tool_call_count >= kHardLimitToolCalls) {
            AgentTraceMetrics::Instance().RecordLongSessionDropped();
            return false;
        }
        st.tool_call_count++;
        st.last_activity_ms = clock_();
        agent_id = st.agent_id;
        namespace_id = st.namespace_id;

        // The 10K-th tool_call crosses the long-session mark (topic 5 — count, no
        // session_id label; the id goes to the structured log at the call site).
        if (st.tool_call_count == kHardLimitToolCalls) {
            AgentTraceMetrics::Instance().RecordLongSession();
        }
    }

    AgentTraceEntry e;
    e.session_id = session_id;
    e.trace_id = uuid_gen_();  // per-tool_call trace_id (C1)
    e.agent_id = agent_id;
    e.namespace_id = namespace_id;
    e.method = call.tool_name;
    e.params = TruncateParams(call.params);
    e.source = "mcp";
    e.duration_ms = result.duration_ms;
    if (result.is_success) {
        e.status = "success";
        e.result_summary = TruncateResult(result.summary);
    } else {
        e.status = "failed";
        e.error_code = result.error_code;
        e.result_summary = FormatError(result.error_code, result.error_message);
    }
    writer_->Write(e);
    return true;
}

void McpSessionHandler::OnConnectionClosed(const std::string& session_id) {
    bool was_registered = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        was_registered = sessions_.erase(session_id) > 0;
    }
    if (!was_registered) return;  // unknown/double-close: nothing to finalize

    // topic 5 — session_end marker row.
    AgentTraceEntry end;
    end.session_id = session_id;
    end.method = "session_end";
    end.source = "mcp";
    end.status = "success";
    writer_->Write(end);
    writer_->OnSessionEnd(session_id);

    AgentTraceMetrics::Instance().DecActiveSessions();
}

int McpSessionHandler::CheckIdleSessions() {
    const int64_t now = clock_();
    const int64_t cutoff = now - static_cast<int64_t>(idle_timeout_seconds_) * 1000LL;

    std::vector<std::string> timed_out;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second.last_activity_ms < cutoff) {
                timed_out.push_back(it->first);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const std::string& sid : timed_out) {
        AgentTraceEntry e;
        e.session_id = sid;
        e.method = "session_timeout";
        e.source = "mcp";
        e.status = "session_timeout";
        writer_->Write(e);
        AgentTraceMetrics::Instance().DecActiveSessions();
    }
    return static_cast<int>(timed_out.size());
}

size_t McpSessionHandler::active_session_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return sessions_.size();
}

}  // namespace cortrix::agent_trace
