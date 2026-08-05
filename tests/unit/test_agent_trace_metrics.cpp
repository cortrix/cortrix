#include <gtest/gtest.h>

#include <string>

#include "cortrix/agent_trace/agent_trace_metrics.h"

// S7 coverage: the 9 cortrix_* agent trace metrics (§13) — counters/gauge/histogram
// recording + the OpenMetrics renderer + label-enum discipline (§3.2 no
// high-cardinality labels; long_session_count_total has no session_id label).
namespace cortrix::agent_trace {
namespace {

using Source = AgentTraceMetrics::Source;
using WriteStatus = AgentTraceMetrics::WriteStatus;
using HeaderName = AgentTraceMetrics::HeaderName;
using Role = AgentTraceMetrics::Role;
using Endpoint = AgentTraceMetrics::Endpoint;
using Module = AgentTraceMetrics::Module;

class AgentTraceMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { AgentTraceMetrics::Instance().ResetForTest(); }
    void TearDown() override { AgentTraceMetrics::Instance().ResetForTest(); }
    AgentTraceMetrics& M() { return AgentTraceMetrics::Instance(); }
};

TEST_F(AgentTraceMetricsTest, WritesCountedBySourceAndStatus) {
    M().RecordWrite(Source::kMcp, WriteStatus::kSuccess);
    M().RecordWrite(Source::kMcp, WriteStatus::kSuccess);
    M().RecordWrite(Source::kMcp, WriteStatus::kFailed);
    M().RecordWrite(Source::kHttp, WriteStatus::kSessionTimeout);
    EXPECT_EQ(M().WriteCount(Source::kMcp, WriteStatus::kSuccess), 2u);
    EXPECT_EQ(M().WriteCount(Source::kMcp, WriteStatus::kFailed), 1u);
    EXPECT_EQ(M().WriteCount(Source::kHttp, WriteStatus::kSessionTimeout), 1u);
    EXPECT_EQ(M().WriteCount(Source::kHttp, WriteStatus::kCancelled), 0u);
}

TEST_F(AgentTraceMetricsTest, InvalidHeaderByName) {
    M().RecordInvalidHeader(HeaderName::kSessionId);
    M().RecordInvalidHeader(HeaderName::kTraceId);
    M().RecordInvalidHeader(HeaderName::kTraceId);
    EXPECT_EQ(M().InvalidHeaderCount(HeaderName::kSessionId), 1u);
    EXPECT_EQ(M().InvalidHeaderCount(HeaderName::kTraceId), 2u);
    EXPECT_EQ(M().InvalidHeaderCount(HeaderName::kAgentId), 0u);
}

TEST_F(AgentTraceMetricsTest, TracesQueryByRoleAndEndpoint) {
    M().RecordTracesQuery(Role::kAdmin, Endpoint::kTraces);
    M().RecordTracesQuery(Role::kUser, Endpoint::kInteractions);
    M().RecordTracesQuery(Role::kUser, Endpoint::kInteractionsSources);
    EXPECT_EQ(M().TracesQueryCount(Role::kAdmin, Endpoint::kTraces), 1u);
    EXPECT_EQ(M().TracesQueryCount(Role::kUser, Endpoint::kInteractions), 1u);
    EXPECT_EQ(M().TracesQueryCount(Role::kUser, Endpoint::kInteractionsSources), 1u);
    EXPECT_EQ(M().TracesQueryCount(Role::kAdmin, Endpoint::kInteractions), 0u);
}

TEST_F(AgentTraceMetricsTest, LongSessionAndDroppedCounters) {
    M().RecordLongSession();
    M().RecordLongSessionDropped();
    M().RecordLongSessionDropped();
    EXPECT_EQ(M().LongSessionCount(), 1u);
    EXPECT_EQ(M().LongSessionDroppedCount(), 2u);
}

TEST_F(AgentTraceMetricsTest, ActiveSessionsGaugeIncDec) {
    M().IncActiveSessions();
    M().IncActiveSessions();
    M().DecActiveSessions();
    EXPECT_EQ(M().ActiveSessions(), 1);
}

TEST_F(AgentTraceMetricsTest, ToolCallRetryAndWriteFailedByModule) {
    M().RecordToolCallRetry();
    M().RecordWriteFailed(Module::kF13);
    M().RecordWriteFailed(Module::kF18a);
    M().RecordWriteFailed(Module::kF18a);
    EXPECT_EQ(M().ToolCallRetryCount(), 1u);
    EXPECT_EQ(M().WriteFailedCount(Module::kF13), 1u);
    EXPECT_EQ(M().WriteFailedCount(Module::kF18a), 2u);
}

TEST_F(AgentTraceMetricsTest, QueryLatencyHistogramSumAndCount) {
    M().ObserveQueryLatency(5);    // 0.005s
    M().ObserveQueryLatency(40);   // 0.04s
    M().ObserveQueryLatency(2000); // 2s → +Inf bucket
    M().ObserveQueryLatency(-1);   // clamped to 0
    EXPECT_EQ(M().QueryLatencyCount(), 4u);

    const std::string out = M().RenderOpenMetrics();
    // count == 4 (sum == (5+40+2000+0)/1000 = 2.045s; exact double formatting is
    // ostream-dependent so we assert count + the cumulative +Inf bucket only).
    EXPECT_NE(out.find("cortrix_agent_trace_query_latency_seconds_count 4"), std::string::npos);
    EXPECT_NE(out.find("cortrix_agent_trace_query_latency_seconds_sum "), std::string::npos);
    // The +Inf bucket is cumulative == total count.
    EXPECT_NE(out.find("cortrix_agent_trace_query_latency_seconds_bucket{le=\"+Inf\"} 4"),
              std::string::npos);
    // The 0.005 bucket holds the single 5ms observation + the clamped 0ms one = 2.
    EXPECT_NE(out.find("cortrix_agent_trace_query_latency_seconds_bucket{le=\"0.005\"} 2"),
              std::string::npos);
}

// The renderer emits all 9 metrics with TYPE lines, and long_session_count_total
// carries NO label (topic 5 v1.0.2 — session_id goes to the structured log).
TEST_F(AgentTraceMetricsTest, RenderEmitsAllNineMetricsWithCorrectCardinality) {
    M().RecordWrite(Source::kHttp, WriteStatus::kSuccess);
    M().RecordTracesQuery(Role::kUser, Endpoint::kTraces);
    M().RecordLongSession();
    M().IncActiveSessions();
    const std::string out = M().RenderOpenMetrics();

    for (const char* name : {"cortrix_agent_trace_writes_total",
                             "cortrix_agent_trace_query_latency_seconds",
                             "cortrix_invalid_header_total",
                             "cortrix_traces_query_total",
                             "cortrix_long_session_count_total",
                             "cortrix_long_session_dropped_total",
                             "cortrix_mcp_active_sessions",
                             "cortrix_tool_call_retry_total",
                             "cortrix_observability_write_failed_total"}) {
        EXPECT_NE(out.find(std::string("# TYPE ") + name), std::string::npos)
            << "missing TYPE for " << name;
    }
    // long_session_count_total is emitted bare (no '{' label set immediately after the name).
    EXPECT_NE(out.find("cortrix_long_session_count_total 1\n"), std::string::npos);
    // gauge value present.
    EXPECT_NE(out.find("cortrix_mcp_active_sessions 1\n"), std::string::npos);
    // labeled series render with the right label keys.
    EXPECT_NE(out.find("cortrix_agent_trace_writes_total{source=\"http\",status=\"success\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_traces_query_total{by_role=\"user\",endpoint=\"traces\"} 1"),
              std::string::npos);
}

TEST_F(AgentTraceMetricsTest, ToStringCoversAllEnumValues) {
    EXPECT_STREQ(ToString(Source::kHttp), "http");
    EXPECT_STREQ(ToString(Source::kMcp), "mcp");
    EXPECT_STREQ(ToString(WriteStatus::kSuccess), "success");
    EXPECT_STREQ(ToString(WriteStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(WriteStatus::kCancelled), "cancelled");
    EXPECT_STREQ(ToString(WriteStatus::kSessionTimeout), "session_timeout");
    EXPECT_STREQ(ToString(HeaderName::kSessionId), "X-Session-Id");
    EXPECT_STREQ(ToString(HeaderName::kTraceId), "X-Trace-Id");
    EXPECT_STREQ(ToString(HeaderName::kAgentId), "X-Agent-Id");
    EXPECT_STREQ(ToString(Role::kUser), "user");
    EXPECT_STREQ(ToString(Role::kAdmin), "admin");
    EXPECT_STREQ(ToString(Endpoint::kTraces), "traces");
    EXPECT_STREQ(ToString(Endpoint::kInteractions), "interactions");
    EXPECT_STREQ(ToString(Endpoint::kInteractionsSources), "interactions_sources");
    EXPECT_STREQ(ToString(Module::kF13), "f13");
    EXPECT_STREQ(ToString(Module::kF18a), "f18a");
}

}  // namespace
}  // namespace cortrix::agent_trace
