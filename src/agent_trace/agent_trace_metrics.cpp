#include <cstdint>
#include "cortrix/agent_trace/agent_trace_metrics.h"

#include <sstream>

namespace cortrix::agent_trace {

namespace {

// query_latency_seconds histogram bucket upper bounds (seconds), §13. Parallel
// string array for rendering exact `le` labels. The trailing +Inf bucket is
// implicit (index kNumDurBuckets). API query latencies are sub-second, so the
// bounds are tighter than the MEM02 extraction histogram.
constexpr double kDurBounds[AgentTraceMetrics::kNumDurBuckets] =
    {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0};
constexpr const char* kDurBoundStr[AgentTraceMetrics::kNumDurBuckets] =
    {"0.005", "0.01", "0.025", "0.05", "0.1", "0.25", "0.5", "1"};

int DurBucketIndex(double seconds) {
    for (int j = 0; j < AgentTraceMetrics::kNumDurBuckets; ++j) {
        if (seconds <= kDurBounds[j]) return j;
    }
    return AgentTraceMetrics::kNumDurBuckets;  // +Inf
}

}  // namespace

AgentTraceMetrics& AgentTraceMetrics::Instance() {
    static AgentTraceMetrics instance;
    return instance;
}

void AgentTraceMetrics::RecordWrite(Source source, WriteStatus status) {
    writes_[static_cast<int>(source)][static_cast<int>(status)].fetch_add(
        1, std::memory_order_relaxed);
}

uint64_t AgentTraceMetrics::WriteCount(Source source, WriteStatus status) const {
    return writes_[static_cast<int>(source)][static_cast<int>(status)].load(
        std::memory_order_relaxed);
}

void AgentTraceMetrics::ObserveQueryLatency(int latency_ms) {
    if (latency_ms < 0) latency_ms = 0;
    q_lat_sum_ms_.fetch_add(static_cast<uint64_t>(latency_ms), std::memory_order_relaxed);
    q_lat_count_.fetch_add(1, std::memory_order_relaxed);
    const int bi = DurBucketIndex(static_cast<double>(latency_ms) / 1000.0);
    q_lat_bkt_[bi].fetch_add(1, std::memory_order_relaxed);
}

uint64_t AgentTraceMetrics::QueryLatencyCount() const {
    return q_lat_count_.load(std::memory_order_relaxed);
}

void AgentTraceMetrics::RecordInvalidHeader(HeaderName header) {
    invalid_header_[static_cast<int>(header)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t AgentTraceMetrics::InvalidHeaderCount(HeaderName header) const {
    return invalid_header_[static_cast<int>(header)].load(std::memory_order_relaxed);
}

void AgentTraceMetrics::RecordTracesQuery(Role role, Endpoint endpoint) {
    traces_query_[static_cast<int>(role)][static_cast<int>(endpoint)].fetch_add(
        1, std::memory_order_relaxed);
}

uint64_t AgentTraceMetrics::TracesQueryCount(Role role, Endpoint endpoint) const {
    return traces_query_[static_cast<int>(role)][static_cast<int>(endpoint)].load(
        std::memory_order_relaxed);
}

void AgentTraceMetrics::RecordLongSession() {
    long_session_.fetch_add(1, std::memory_order_relaxed);
}
uint64_t AgentTraceMetrics::LongSessionCount() const {
    return long_session_.load(std::memory_order_relaxed);
}

void AgentTraceMetrics::RecordLongSessionDropped() {
    long_session_dropped_.fetch_add(1, std::memory_order_relaxed);
}
uint64_t AgentTraceMetrics::LongSessionDroppedCount() const {
    return long_session_dropped_.load(std::memory_order_relaxed);
}

void AgentTraceMetrics::IncActiveSessions() {
    active_sessions_.fetch_add(1, std::memory_order_relaxed);
}
void AgentTraceMetrics::DecActiveSessions() {
    active_sessions_.fetch_sub(1, std::memory_order_relaxed);
}
int64_t AgentTraceMetrics::ActiveSessions() const {
    return active_sessions_.load(std::memory_order_relaxed);
}

void AgentTraceMetrics::RecordToolCallRetry() {
    tool_call_retry_.fetch_add(1, std::memory_order_relaxed);
}
uint64_t AgentTraceMetrics::ToolCallRetryCount() const {
    return tool_call_retry_.load(std::memory_order_relaxed);
}

void AgentTraceMetrics::RecordWriteFailed(Module module) {
    write_failed_[static_cast<int>(module)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t AgentTraceMetrics::WriteFailedCount(Module module) const {
    return write_failed_[static_cast<int>(module)].load(std::memory_order_relaxed);
}

std::string AgentTraceMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_agent_trace_writes_total {source, status}
    os << "# HELP cortrix_agent_trace_writes_total Total agent_trace writes by source and status.\n";
    os << "# TYPE cortrix_agent_trace_writes_total counter\n";
    for (int s = 0; s < kSourceCount; ++s) {
        for (int st = 0; st < kWriteStatusCount; ++st) {
            os << "cortrix_agent_trace_writes_total{source=\""
               << ToString(static_cast<Source>(s)) << "\",status=\""
               << ToString(static_cast<WriteStatus>(st)) << "\"} "
               << writes_[s][st].load(std::memory_order_relaxed) << "\n";
        }
    }

    // cortrix_agent_trace_query_latency_seconds (histogram, no label)
    os << "# HELP cortrix_agent_trace_query_latency_seconds Trace query latency in seconds.\n";
    os << "# TYPE cortrix_agent_trace_query_latency_seconds histogram\n";
    {
        uint64_t cum = 0;
        for (int b = 0; b < kNumDurBuckets; ++b) {
            cum += q_lat_bkt_[b].load(std::memory_order_relaxed);
            os << "cortrix_agent_trace_query_latency_seconds_bucket{le=\""
               << kDurBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += q_lat_bkt_[kNumDurBuckets].load(std::memory_order_relaxed);
        os << "cortrix_agent_trace_query_latency_seconds_bucket{le=\"+Inf\"} " << cum << "\n";
        const double sum_s =
            static_cast<double>(q_lat_sum_ms_.load(std::memory_order_relaxed)) / 1000.0;
        os << "cortrix_agent_trace_query_latency_seconds_sum " << sum_s << "\n";
        os << "cortrix_agent_trace_query_latency_seconds_count "
           << q_lat_count_.load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_invalid_header_total {header_name}
    os << "# HELP cortrix_invalid_header_total Invalid observability headers by name.\n";
    os << "# TYPE cortrix_invalid_header_total counter\n";
    for (int i = 0; i < kHeaderCount; ++i) {
        os << "cortrix_invalid_header_total{header_name=\""
           << ToString(static_cast<HeaderName>(i)) << "\"} "
           << invalid_header_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_traces_query_total {by_role, endpoint}
    os << "# HELP cortrix_traces_query_total Read-endpoint queries by role and endpoint.\n";
    os << "# TYPE cortrix_traces_query_total counter\n";
    for (int r = 0; r < kRoleCount; ++r) {
        for (int e = 0; e < kEndpointCount; ++e) {
            os << "cortrix_traces_query_total{by_role=\""
               << ToString(static_cast<Role>(r)) << "\",endpoint=\""
               << ToString(static_cast<Endpoint>(e)) << "\"} "
               << traces_query_[r][e].load(std::memory_order_relaxed) << "\n";
        }
    }

    // cortrix_long_session_count_total (no label — session_id → structured log)
    os << "# HELP cortrix_long_session_count_total MCP sessions exceeding the 10K tool_call mark.\n";
    os << "# TYPE cortrix_long_session_count_total counter\n";
    os << "cortrix_long_session_count_total " << long_session_.load(std::memory_order_relaxed) << "\n";

    // cortrix_long_session_dropped_total
    os << "# HELP cortrix_long_session_dropped_total Traces dropped past the 10K hard limit.\n";
    os << "# TYPE cortrix_long_session_dropped_total counter\n";
    os << "cortrix_long_session_dropped_total "
       << long_session_dropped_.load(std::memory_order_relaxed) << "\n";

    // cortrix_mcp_active_sessions (gauge)
    os << "# HELP cortrix_mcp_active_sessions Currently open MCP sessions.\n";
    os << "# TYPE cortrix_mcp_active_sessions gauge\n";
    os << "cortrix_mcp_active_sessions " << active_sessions_.load(std::memory_order_relaxed) << "\n";

    // cortrix_tool_call_retry_total
    os << "# HELP cortrix_tool_call_retry_total Trace-write retries.\n";
    os << "# TYPE cortrix_tool_call_retry_total counter\n";
    os << "cortrix_tool_call_retry_total " << tool_call_retry_.load(std::memory_order_relaxed) << "\n";

    // cortrix_observability_write_failed_total {module} (C4 coordination)
    os << "# HELP cortrix_observability_write_failed_total Observability write failures by module.\n";
    os << "# TYPE cortrix_observability_write_failed_total counter\n";
    for (int i = 0; i < kModuleCount; ++i) {
        os << "cortrix_observability_write_failed_total{module=\""
           << ToString(static_cast<Module>(i)) << "\"} "
           << write_failed_[i].load(std::memory_order_relaxed) << "\n";
    }

    return os.str();
}

void AgentTraceMetrics::ResetForTest() {
    for (auto& row : writes_)
        for (auto& a : row) a.store(0, std::memory_order_relaxed);
    for (auto& row : traces_query_)
        for (auto& a : row) a.store(0, std::memory_order_relaxed);
    for (auto& a : invalid_header_) a.store(0, std::memory_order_relaxed);
    for (auto& a : write_failed_) a.store(0, std::memory_order_relaxed);
    long_session_.store(0, std::memory_order_relaxed);
    long_session_dropped_.store(0, std::memory_order_relaxed);
    tool_call_retry_.store(0, std::memory_order_relaxed);
    active_sessions_.store(0, std::memory_order_relaxed);
    q_lat_sum_ms_.store(0, std::memory_order_relaxed);
    q_lat_count_.store(0, std::memory_order_relaxed);
    for (auto& b : q_lat_bkt_) b.store(0, std::memory_order_relaxed);
}

const char* ToString(AgentTraceMetrics::Source source) {
    switch (source) {
        case AgentTraceMetrics::Source::kHttp: return "http";
        case AgentTraceMetrics::Source::kMcp:  return "mcp";
    }
    return "unknown";
}

const char* ToString(AgentTraceMetrics::WriteStatus status) {
    switch (status) {
        case AgentTraceMetrics::WriteStatus::kSuccess:        return "success";
        case AgentTraceMetrics::WriteStatus::kFailed:         return "failed";
        case AgentTraceMetrics::WriteStatus::kCancelled:      return "cancelled";
        case AgentTraceMetrics::WriteStatus::kSessionTimeout: return "session_timeout";
    }
    return "unknown";
}

const char* ToString(AgentTraceMetrics::HeaderName header) {
    switch (header) {
        case AgentTraceMetrics::HeaderName::kSessionId: return "X-Session-Id";
        case AgentTraceMetrics::HeaderName::kTraceId:   return "X-Trace-Id";
        case AgentTraceMetrics::HeaderName::kAgentId:   return "X-Agent-Id";
    }
    return "unknown";
}

const char* ToString(AgentTraceMetrics::Role role) {
    switch (role) {
        case AgentTraceMetrics::Role::kUser:  return "user";
        case AgentTraceMetrics::Role::kAdmin: return "admin";
    }
    return "unknown";
}

const char* ToString(AgentTraceMetrics::Endpoint endpoint) {
    switch (endpoint) {
        case AgentTraceMetrics::Endpoint::kTraces:              return "traces";
        case AgentTraceMetrics::Endpoint::kInteractions:        return "interactions";
        case AgentTraceMetrics::Endpoint::kInteractionsSources: return "interactions_sources";
    }
    return "unknown";
}

const char* ToString(AgentTraceMetrics::Module module) {
    switch (module) {
        case AgentTraceMetrics::Module::kF13:  return "f13";
        case AgentTraceMetrics::Module::kF18a: return "f18a";
    }
    return "unknown";
}

}  // namespace cortrix::agent_trace
