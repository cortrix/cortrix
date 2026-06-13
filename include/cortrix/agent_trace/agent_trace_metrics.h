#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::agent_trace {

/// The F13 OBS_SPEC metrics (F13 §13 = 9 metrics, OBSERVABILITY_SPEC §2 naming
/// `cortrix_<metric>_<unit>`). Mirrors the MEM02 Mem02Metrics / F36 RagFusionMetrics
/// template (process-wide singleton, atomic counters/gauge/histogram, OpenMetrics
/// renderer).
///
/// Cardinality control (OBSERVABILITY_SPEC §3.2 — C5 decision): labels are enum-only
/// + low-cardinality. NO session_id / agent_id / user_id / namespace_id labels
/// (high-cardinality, forbidden) — long_session_count_total in particular drops
/// the session_id label (it goes to the structured log, topic 5 v1.0.2).
///
/// 🚨 D3 standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer. The F24 `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder into that endpoint is cross-Feature wiring
/// **deferred to D3.5**. Until then it is fully usable + testable in-process and
/// RenderOpenMetrics() produces what F24 will serve.
///
/// §13 metric schema (9 rows):
///   cortrix_agent_trace_writes_total           counter   {source, status}
///   cortrix_agent_trace_query_latency_seconds  histogram (no label)
///   cortrix_invalid_header_total               counter   {header_name}
///   cortrix_traces_query_total                 counter   {by_role, endpoint}
///   cortrix_long_session_count_total           counter   (no label — session_id → structured log)
///   cortrix_long_session_dropped_total         counter   (no label)
///   cortrix_mcp_active_sessions                gauge     (no label)
///   cortrix_tool_call_retry_total              counter   (no label)
///   cortrix_observability_write_failed_total   counter   {module}
class AgentTraceMetrics {
public:
    /// source label for agent_trace_writes_total (§4.1).
    enum class Source { kHttp = 0, kMcp };

    /// status label for agent_trace_writes_total (§4.1).
    enum class WriteStatus { kSuccess = 0, kFailed, kCancelled, kSessionTimeout };

    /// header_name label for invalid_header_total (§6.1).
    enum class HeaderName { kSessionId = 0, kTraceId, kAgentId };

    /// by_role label for traces_query_total (§8.x permission tiers).
    enum class Role { kUser = 0, kAdmin };

    /// endpoint label for traces_query_total (3 read endpoints).
    enum class Endpoint { kTraces = 0, kInteractions, kInteractionsSources };

    /// module label for observability_write_failed_total (C4 coordination — F13 / F18a).
    enum class Module { kF13 = 0, kF18a };

    /// Process-wide instance (metrics are global counters/gauge/histogram).
    static AgentTraceMetrics& Instance();

    // --- cortrix_agent_trace_writes_total (Counter, labels: source, status) ---
    void RecordWrite(Source source, WriteStatus status);
    uint64_t WriteCount(Source source, WriteStatus status) const;

    // --- cortrix_agent_trace_query_latency_seconds (Histogram, no label) ---
    void ObserveQueryLatency(int latency_ms);
    uint64_t QueryLatencyCount() const;

    // --- cortrix_invalid_header_total (Counter, label: header_name) ---
    void RecordInvalidHeader(HeaderName header);
    uint64_t InvalidHeaderCount(HeaderName header) const;

    // --- cortrix_traces_query_total (Counter, labels: by_role, endpoint) ---
    void RecordTracesQuery(Role role, Endpoint endpoint);
    uint64_t TracesQueryCount(Role role, Endpoint endpoint) const;

    // --- cortrix_long_session_count_total (Counter, no label) ---
    void RecordLongSession();
    uint64_t LongSessionCount() const;

    // --- cortrix_long_session_dropped_total (Counter, no label) ---
    void RecordLongSessionDropped();
    uint64_t LongSessionDroppedCount() const;

    // --- cortrix_mcp_active_sessions (Gauge, no label) ---
    void IncActiveSessions();
    void DecActiveSessions();
    int64_t ActiveSessions() const;

    // --- cortrix_tool_call_retry_total (Counter, no label) ---
    void RecordToolCallRetry();
    uint64_t ToolCallRetryCount() const;

    // --- cortrix_observability_write_failed_total (Counter, label: module) ---
    void RecordWriteFailed(Module module);
    uint64_t WriteFailedCount(Module module) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauge/histogram (test-only — production metrics are monotonic).
    void ResetForTest();

    /// Number of explicit `le` buckets in the query_latency_seconds histogram
    /// (a trailing +Inf bucket is implicit).
    static constexpr int kNumDurBuckets = 8;  // {0.005,0.01,0.025,0.05,0.1,0.25,0.5,1}

private:
    AgentTraceMetrics() = default;

    static constexpr int kSourceCount = 2;       // http / mcp
    static constexpr int kWriteStatusCount = 4;  // success / failed / cancelled / session_timeout
    static constexpr int kHeaderCount = 3;       // session_id / trace_id / agent_id
    static constexpr int kRoleCount = 2;         // user / admin
    static constexpr int kEndpointCount = 3;     // traces / interactions / interactions_sources
    static constexpr int kModuleCount = 2;       // f13 / f18a

    // writes_total[source][status]
    std::array<std::array<std::atomic<uint64_t>, kWriteStatusCount>, kSourceCount> writes_{};
    // traces_query_total[role][endpoint]
    std::array<std::array<std::atomic<uint64_t>, kEndpointCount>, kRoleCount> traces_query_{};
    std::array<std::atomic<uint64_t>, kHeaderCount> invalid_header_{};
    std::array<std::atomic<uint64_t>, kModuleCount> write_failed_{};
    std::atomic<uint64_t> long_session_{0};
    std::atomic<uint64_t> long_session_dropped_{0};
    std::atomic<uint64_t> tool_call_retry_{0};
    std::atomic<int64_t> active_sessions_{0};

    // query_latency_seconds histogram (single series, no label).
    std::atomic<uint64_t> q_lat_sum_ms_{0};
    std::atomic<uint64_t> q_lat_count_{0};
    std::array<std::atomic<uint64_t>, kNumDurBuckets + 1> q_lat_bkt_{};  // last = +Inf
};

const char* ToString(AgentTraceMetrics::Source source);
const char* ToString(AgentTraceMetrics::WriteStatus status);
const char* ToString(AgentTraceMetrics::HeaderName header);
const char* ToString(AgentTraceMetrics::Role role);
const char* ToString(AgentTraceMetrics::Endpoint endpoint);
const char* ToString(AgentTraceMetrics::Module module);

}  // namespace cortrix::agent_trace
