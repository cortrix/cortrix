#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/engine_instrumentation.h"
#include "cortrix/observability/observability_context.h"

// S6 coverage: Engine-layer instrumentation (§11, topic 8 C4) — identity read from
// the thread-local ObservabilityContext, trace-first / operation-log-second-on-success
// ordering, and C4 exception isolation (a throwing writer never propagates; the
// write_failed metric is bumped instead).
namespace cortrix::agent_trace {
namespace {

using observability::ObservabilityContext;
using observability::TraceContext;

// A capturing mock trace writer; optionally throws on Write to exercise C4.
class MockTraceWriter : public IAgentTraceWriter {
public:
    bool throw_on_write = false;
    std::vector<AgentTraceEntry> written;

    void Write(const AgentTraceEntry& e, const observability::TraceContext*) override {
        if (throw_on_write) throw std::runtime_error("boom");
        written.push_back(e);
    }
    Result<TraceSession> Query(const std::string&, const TraceFilter&,
                               const observability::TraceContext*) override {
        return Status::Internal("unused");
    }
    void OnSessionEnd(const std::string&, const observability::TraceContext*) override {}
    void CheckAndMarkTimeoutSessions(int, const observability::TraceContext*) override {}
    void Cleanup() override {}
};

// A capturing mock operation logger; optionally throws on Log.
class MockOpLogger : public observability::IOperationLogger {
public:
    bool throw_on_log = false;
    std::vector<observability::OperationLogEntry> logged;

    void Log(const observability::OperationLogEntry& e,
             const observability::TraceContext*) override {
        if (throw_on_log) throw std::runtime_error("boom");
        logged.push_back(e);
    }
    void BatchLog(const std::vector<observability::OperationLogEntry>&,
                  const observability::TraceContext*) override {}
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext*) override {
        return Status::Internal("unused");
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }
};

class EngineInstrumentationTest : public ::testing::Test {
protected:
    void SetUp() override {
        AgentTraceMetrics::Instance().ResetForTest();
        // Seed the thread-local identity context (C1) the helper reads.
        auto& ctx = ObservabilityContext::ThreadLocal();
        ctx.ClearTraceContext();
        ctx.SetTraceContext(TraceContext{"trace-77", "span-1", 0});
        ctx.session_id = "sess-77";
        ctx.agent_id = "bot-77";
        ctx.user_id = "alice";
    }
    void TearDown() override {
        auto& ctx = ObservabilityContext::ThreadLocal();
        ctx.ClearTraceContext();
        ctx.session_id.reset();
        ctx.agent_id.reset();
        ctx.user_id.reset();
        AgentTraceMetrics::Instance().ResetForTest();
    }
};

TEST_F(EngineInstrumentationTest, SuccessWritesBothWithIdentityFromContext) {
    auto tw = std::make_shared<MockTraceWriter>();
    auto ol = std::make_shared<MockOpLogger>();
    EngineInstrumentation instr(tw, ol);

    EngineCall c;
    c.method = "query";
    c.source = "http";
    c.namespace_id = "sales";
    c.params_json = R"({"q":"hi"})";
    c.is_success = true;
    c.result_summary = "5 chunks";
    c.duration_ms = 50;
    c.resource_type = "query";
    c.resource_id = "int-9";
    c.op_summary = "asked about coffee";
    instr.Record(c);

    // agent_trace row carries the thread-local identity.
    ASSERT_EQ(tw->written.size(), 1u);
    const auto& e = tw->written[0];
    EXPECT_EQ(e.session_id, "sess-77");
    EXPECT_EQ(e.agent_id, "bot-77");
    EXPECT_EQ(e.trace_id, "trace-77");
    EXPECT_EQ(e.method, "query");
    EXPECT_EQ(e.status, "success");
    EXPECT_EQ(e.namespace_id, "sales");

    // operation_log written on success, with user_id from context.
    ASSERT_EQ(ol->logged.size(), 1u);
    EXPECT_EQ(ol->logged[0].user_id, "alice");
    EXPECT_EQ(ol->logged[0].action, "query");
    EXPECT_EQ(ol->logged[0].trace_id, "trace-77");
    EXPECT_EQ(ol->logged[0].session_id, "sess-77");
    EXPECT_EQ(ol->logged[0].resource_id, "int-9");
}

TEST_F(EngineInstrumentationTest, FailureWritesTraceOnlyNoOperationLog) {
    auto tw = std::make_shared<MockTraceWriter>();
    auto ol = std::make_shared<MockOpLogger>();
    EngineInstrumentation instr(tw, ol);

    EngineCall c;
    c.method = "query";
    c.is_success = false;
    c.error_code = "CX_ERR_NS_NOT_FOUND";
    c.error_message = "no such namespace";
    instr.Record(c);

    // agent_trace written (failed); operation_log NOT (topic 8 — success only).
    ASSERT_EQ(tw->written.size(), 1u);
    EXPECT_EQ(tw->written[0].status, "failed");
    EXPECT_EQ(tw->written[0].error_code, "CX_ERR_NS_NOT_FOUND");
    EXPECT_NE(tw->written[0].result_summary->find("CX_ERR_NS_NOT_FOUND"), std::string::npos);
    EXPECT_TRUE(ol->logged.empty());
}

TEST_F(EngineInstrumentationTest, ThrowingTraceWriterIsIsolated) {
    auto tw = std::make_shared<MockTraceWriter>();
    tw->throw_on_write = true;
    auto ol = std::make_shared<MockOpLogger>();
    EngineInstrumentation instr(tw, ol);

    EngineCall c;
    c.method = "query";
    c.is_success = true;
    // Must NOT throw out of Record (C4 isolation).
    EXPECT_NO_THROW(instr.Record(c));
    // write_failed{agent_trace} bumped; operation_log still attempted (success path) and ok.
    EXPECT_EQ(AgentTraceMetrics::Instance().WriteFailedCount(
                  AgentTraceMetrics::Module::kAgentTrace), 1u);
    EXPECT_EQ(ol->logged.size(), 1u);
}

TEST_F(EngineInstrumentationTest, ThrowingOpLoggerIsIsolated) {
    auto tw = std::make_shared<MockTraceWriter>();
    auto ol = std::make_shared<MockOpLogger>();
    ol->throw_on_log = true;
    EngineInstrumentation instr(tw, ol);

    EngineCall c;
    c.method = "query";
    c.is_success = true;
    EXPECT_NO_THROW(instr.Record(c));
    // agent_trace still written; write_failed{oplog} bumped.
    EXPECT_EQ(tw->written.size(), 1u);
    EXPECT_EQ(AgentTraceMetrics::Instance().WriteFailedCount(
                  AgentTraceMetrics::Module::kOperationLog), 1u);
}

TEST_F(EngineInstrumentationTest, NoOpLoggerTracesOnly) {
    auto tw = std::make_shared<MockTraceWriter>();
    EngineInstrumentation instr(tw);  // no operation logger

    EngineCall c;
    c.method = "upload";
    c.is_success = true;
    EXPECT_NO_THROW(instr.Record(c));
    EXPECT_EQ(tw->written.size(), 1u);
    // No operation logger => no write_failed for oplog, no crash.
    EXPECT_EQ(AgentTraceMetrics::Instance().WriteFailedCount(
                  AgentTraceMetrics::Module::kOperationLog), 0u);
}

}  // namespace
}  // namespace cortrix::agent_trace
