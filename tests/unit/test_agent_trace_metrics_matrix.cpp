// AgentTraceMetrics — exhaustive matrix coverage (enum ToString, (source x
// status) / (role x endpoint) / module / header label combos, latency histogram,
// gauge, RenderOpenMetrics series + no high-cardinality labels, ResetForTest).
// Separate suite namespace from test_agent_trace_metrics.cpp.
#include <gtest/gtest.h>

#include <array>
#include <string>

#include "cortrix/agent_trace/agent_trace_metrics.h"

namespace cortrix::agent_trace {
namespace matrix_test {

using S = AgentTraceMetrics::Source;
using W = AgentTraceMetrics::WriteStatus;
using H = AgentTraceMetrics::HeaderName;
using R = AgentTraceMetrics::Role;
using E = AgentTraceMetrics::Endpoint;
using M = AgentTraceMetrics::Module;

constexpr std::array<S, 2> kSources{S::kHttp, S::kMcp};
constexpr std::array<W, 4> kStatuses{W::kSuccess, W::kFailed, W::kCancelled,
                                     W::kSessionTimeout};
constexpr std::array<R, 2> kRoles{R::kUser, R::kAdmin};
constexpr std::array<E, 3> kEndpoints{E::kTraces, E::kInteractions,
                                      E::kInteractionsSources};
constexpr std::array<H, 3> kHeaders{H::kSessionId, H::kTraceId, H::kAgentId};
constexpr std::array<M, 2> kModules{M::kAgentTrace, M::kOperationLog};

class AgentTraceFx : public ::testing::Test {
protected:
    void SetUp() override { AgentTraceMetrics::Instance().ResetForTest(); }
    void TearDown() override { AgentTraceMetrics::Instance().ResetForTest(); }
    AgentTraceMetrics& m() { return AgentTraceMetrics::Instance(); }
};

// ---- enum ToString -----------------------------------------------------------
TEST_F(AgentTraceFx, ToStringSource) {
    EXPECT_STREQ(ToString(S::kHttp), "http");
    EXPECT_STREQ(ToString(S::kMcp), "mcp");
}
TEST_F(AgentTraceFx, ToStringWriteStatus) {
    EXPECT_STREQ(ToString(W::kSuccess), "success");
    EXPECT_STREQ(ToString(W::kFailed), "failed");
    EXPECT_STREQ(ToString(W::kCancelled), "cancelled");
    EXPECT_STREQ(ToString(W::kSessionTimeout), "session_timeout");
}
TEST_F(AgentTraceFx, ToStringHeader) {
    EXPECT_STREQ(ToString(H::kSessionId), "X-Session-Id");
    EXPECT_STREQ(ToString(H::kTraceId), "X-Trace-Id");
    EXPECT_STREQ(ToString(H::kAgentId), "X-Agent-Id");
}
TEST_F(AgentTraceFx, ToStringRole) {
    EXPECT_STREQ(ToString(R::kUser), "user");
    EXPECT_STREQ(ToString(R::kAdmin), "admin");
}
TEST_F(AgentTraceFx, ToStringEndpoint) {
    EXPECT_STREQ(ToString(E::kTraces), "traces");
    EXPECT_STREQ(ToString(E::kInteractions), "interactions");
    EXPECT_STREQ(ToString(E::kInteractionsSources), "interactions_sources");
}
TEST_F(AgentTraceFx, ToStringModule) {
    EXPECT_STREQ(ToString(M::kAgentTrace), "agent_trace");
    EXPECT_STREQ(ToString(M::kOperationLog), "oplog");
}

// ---- writes_total: every (source x status) combo independent --------------
using WriteParam = std::tuple<S, W>;
class AgentTraceWriteMatrix : public AgentTraceFx, public ::testing::WithParamInterface<WriteParam> {};

TEST_P(AgentTraceWriteMatrix, IncrementsOnlyTargetCell) {
    auto [src, st] = GetParam();
    m().RecordWrite(src, st);
    m().RecordWrite(src, st);
    EXPECT_EQ(m().WriteCount(src, st), 2u);
    // every other cell stays zero
    for (S s : kSources)
        for (W w : kStatuses)
            if (!(s == src && w == st)) EXPECT_EQ(m().WriteCount(s, w), 0u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, AgentTraceWriteMatrix,
                         ::testing::Combine(::testing::ValuesIn(kSources),
                                            ::testing::ValuesIn(kStatuses)));

// ---- traces_query_total: every (role x endpoint) combo --------------------
using QueryParam = std::tuple<R, E>;
class AgentTraceQueryMatrix : public AgentTraceFx, public ::testing::WithParamInterface<QueryParam> {};

TEST_P(AgentTraceQueryMatrix, IncrementsOnlyTargetCell) {
    auto [role, ep] = GetParam();
    m().RecordTracesQuery(role, ep);
    EXPECT_EQ(m().TracesQueryCount(role, ep), 1u);
    for (R r : kRoles)
        for (E e : kEndpoints)
            if (!(r == role && e == ep)) EXPECT_EQ(m().TracesQueryCount(r, e), 0u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, AgentTraceQueryMatrix,
                         ::testing::Combine(::testing::ValuesIn(kRoles),
                                            ::testing::ValuesIn(kEndpoints)));

// ---- per-header / per-module single-label counters ------------------------
class AgentTraceHeaderMatrix : public AgentTraceFx, public ::testing::WithParamInterface<H> {};
TEST_P(AgentTraceHeaderMatrix, Increments) {
    m().RecordInvalidHeader(GetParam());
    m().RecordInvalidHeader(GetParam());
    EXPECT_EQ(m().InvalidHeaderCount(GetParam()), 2u);
}
INSTANTIATE_TEST_SUITE_P(All, AgentTraceHeaderMatrix, ::testing::ValuesIn(kHeaders));

class AgentTraceModuleMatrix : public AgentTraceFx, public ::testing::WithParamInterface<M> {};
TEST_P(AgentTraceModuleMatrix, Increments) {
    m().RecordWriteFailed(GetParam());
    EXPECT_EQ(m().WriteFailedCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, AgentTraceModuleMatrix, ::testing::ValuesIn(kModules));

// ---- no-label counters ----------------------------------------------------
TEST_F(AgentTraceFx, NoLabelCounters) {
    m().RecordLongSession();
    m().RecordLongSession();
    m().RecordLongSessionDropped();
    m().RecordToolCallRetry();
    EXPECT_EQ(m().LongSessionCount(), 2u);
    EXPECT_EQ(m().LongSessionDroppedCount(), 1u);
    EXPECT_EQ(m().ToolCallRetryCount(), 1u);
}

// ---- gauge: active sessions inc/dec ---------------------------------------
TEST_F(AgentTraceFx, GaugeIncDec) {
    EXPECT_EQ(m().ActiveSessions(), 0);
    m().IncActiveSessions();
    m().IncActiveSessions();
    m().IncActiveSessions();
    EXPECT_EQ(m().ActiveSessions(), 3);
    m().DecActiveSessions();
    EXPECT_EQ(m().ActiveSessions(), 2);
}

// ---- histogram: bucket boundary placement ---------------------------------
// query_latency_seconds le bounds {5,10,25,50,100,250,500,1000} ms.
TEST_F(AgentTraceFx, HistogramCountAndSum) {
    m().ObserveQueryLatency(3);
    m().ObserveQueryLatency(20);
    m().ObserveQueryLatency(5000);  // above largest bound -> only +Inf
    EXPECT_EQ(m().QueryLatencyCount(), 3u);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("cortrix_agent_trace_query_latency_seconds_count 3"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_agent_trace_query_latency_seconds_sum"),
              std::string::npos);
}

TEST_F(AgentTraceFx, HistogramBoundaryAtExactBound) {
    // a value exactly at a bound (5ms = 0.005s, the first le) and just under.
    m().ObserveQueryLatency(5);  // exactly first bound
    m().ObserveQueryLatency(4);  // just under
    std::string out = m().RenderOpenMetrics();
    // both fall into le="0.005" -> cumulative bucket reads 2.
    EXPECT_NE(out.find("le=\"0.005\"} 2"), std::string::npos);
    // +Inf bucket equals total count.
    EXPECT_NE(out.find("le=\"+Inf\"} 2"), std::string::npos);
}

TEST_F(AgentTraceFx, HistogramNegativeClampsToSmallestBucket) {
    m().ObserveQueryLatency(-50);  // negative -> clamped, lands in smallest bucket
    EXPECT_EQ(m().QueryLatencyCount(), 1u);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("le=\"0.005\"} 1"), std::string::npos);
    EXPECT_NE(out.find("le=\"+Inf\"} 1"), std::string::npos);
}

TEST_F(AgentTraceFx, HistogramAboveLargestOnlyInf) {
    m().ObserveQueryLatency(999999);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("le=\"1\"} 0"), std::string::npos);     // largest finite empty
    EXPECT_NE(out.find("le=\"+Inf\"} 1"), std::string::npos);  // only +Inf
}

// ---- render: HELP/TYPE present, stable names, no high-cardinality labels ---
TEST_F(AgentTraceFx, RenderHasHelpTypeAndStableSeries) {
    m().RecordWrite(S::kMcp, W::kSuccess);
    m().RecordTracesQuery(R::kAdmin, E::kTraces);
    m().IncActiveSessions();
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_agent_trace_writes_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_agent_trace_writes_total{source=\"mcp\",status=\"success\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_mcp_active_sessions gauge"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_traces_query_total{by_role=\"admin\",endpoint=\"traces\"} 1"),
              std::string::npos);
}

TEST_F(AgentTraceFx, RenderNoHighCardinalityLabels) {
    m().RecordWrite(S::kHttp, W::kFailed);
    std::string out = m().RenderOpenMetrics();
    EXPECT_EQ(out.find("session_id="), std::string::npos);
    EXPECT_EQ(out.find("agent_id="), std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
    EXPECT_EQ(out.find("namespace_id="), std::string::npos);
}

// ---- reset ----------------------------------------------------------------
TEST_F(AgentTraceFx, ResetClearsEverything) {
    m().RecordWrite(S::kHttp, W::kSuccess);
    m().RecordTracesQuery(R::kUser, E::kTraces);
    m().RecordInvalidHeader(H::kSessionId);
    m().RecordWriteFailed(M::kAgentTrace);
    m().RecordLongSession();
    m().RecordLongSessionDropped();
    m().RecordToolCallRetry();
    m().IncActiveSessions();
    m().ObserveQueryLatency(10);
    m().ResetForTest();
    EXPECT_EQ(m().WriteCount(S::kHttp, W::kSuccess), 0u);
    EXPECT_EQ(m().TracesQueryCount(R::kUser, E::kTraces), 0u);
    EXPECT_EQ(m().InvalidHeaderCount(H::kSessionId), 0u);
    EXPECT_EQ(m().WriteFailedCount(M::kAgentTrace), 0u);
    EXPECT_EQ(m().LongSessionCount(), 0u);
    EXPECT_EQ(m().LongSessionDroppedCount(), 0u);
    EXPECT_EQ(m().ToolCallRetryCount(), 0u);
    EXPECT_EQ(m().ActiveSessions(), 0);
    EXPECT_EQ(m().QueryLatencyCount(), 0u);
}

}  // namespace matrix_test
}  // namespace cortrix::agent_trace
