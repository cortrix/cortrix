#include <gtest/gtest.h>

#include <sqlite3.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/agent_trace_writer_impl.h"
#include "cortrix/agent_trace/mcp_session_handler.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/i_global_config.h"

// S3 coverage: the MCP session handler (§7.1) — session_id resolution (generated
// vs validated client id), per-tool_call agent_trace double-write, the Phase-1
// 10K hard limit (drop + metric), session_end on close, idle-timeout backstop,
// and the §3 truncation helpers.
namespace cortrix::agent_trace {
namespace {

class StubGlobalConfig : public cortrix::IGlobalConfig {
public:
    Result<std::string> GetString(const std::string&) const override {
        return Status::InvalidArgument("unused");
    }
    Result<bool> GetBool(const std::string&) const override {
        return Status::InvalidArgument("unused");
    }
    Result<int> GetInt(const std::string&) const override {
        return Status::InvalidArgument("unused");
    }
    Result<float> GetFloat(const std::string&) const override {
        return Status::InvalidArgument("unused");
    }
    void OnChange(std::function<void(const std::string&)>) override {}
};

int64_t QueryInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    int64_t v = -1;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

class McpSessionHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        AgentTraceMetrics::Instance().ResetForTest();
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        config_ = std::make_shared<StubGlobalConfig>();
        writer_ = std::make_shared<AgentTraceWriterImpl>(db_, config_);
        seq_.store(0);
        clock_ms_.store(1'000'000);
    }
    void TearDown() override {
        writer_.reset();
        if (db_) sqlite3_close(db_);
        AgentTraceMetrics::Instance().ResetForTest();
    }

    // Deterministic uuid generator: "uuid-N".
    std::string NextUuid() { return "uuid-" + std::to_string(seq_.fetch_add(1)); }
    int64_t Clock() { return clock_ms_.load(); }

    std::unique_ptr<McpSessionHandler> MakeHandler(int idle_timeout_seconds) {
        return std::make_unique<McpSessionHandler>(
            writer_, idle_timeout_seconds,
            [this] { return NextUuid(); },
            [this] { return Clock(); });
    }

    AgentTraceSchemaProvider provider_;
    std::shared_ptr<StubGlobalConfig> config_;
    std::shared_ptr<AgentTraceWriterImpl> writer_;
    std::atomic<int> seq_{0};
    std::atomic<int64_t> clock_ms_{1'000'000};
    sqlite3* db_ = nullptr;
};

TEST_F(McpSessionHandlerTest, GeneratesSessionIdWhenClientHasNone) {
    auto h = MakeHandler(1800);
    McpClientCapability cap;  // no client_session_id
    std::string sid = h->OnConnectionEstablished(cap);
    EXPECT_EQ(sid, "uuid-0");
    EXPECT_EQ(h->active_session_count(), 1u);
    EXPECT_EQ(AgentTraceMetrics::Instance().ActiveSessions(), 1);
}

TEST_F(McpSessionHandlerTest, KeepsValidClientSessionIdRejectsInvalid) {
    auto h = MakeHandler(1800);
    McpClientCapability good;
    good.client_session_id = "client-sess-1";
    EXPECT_EQ(h->OnConnectionEstablished(good), "client-sess-1");

    McpClientCapability bad;
    bad.client_session_id = "bad session id";  // space → invalid → generated
    EXPECT_EQ(h->OnConnectionEstablished(bad), "uuid-0");
}

TEST_F(McpSessionHandlerTest, ToolCallWritesTraceWithPerCallTraceId) {
    auto h = MakeHandler(1800);
    McpClientCapability cap;
    cap.agent_id = "bot-1";
    std::string sid = h->OnConnectionEstablished(cap);  // uuid-0

    McpToolCall call{"cortrix_query", R"({"q":"hi"})"};
    McpToolResult res;
    res.is_success = true;
    res.summary = "5 chunks";
    res.duration_ms = 42;
    EXPECT_TRUE(h->OnToolCall(sid, call, res));

    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE method='cortrix_query'"), 1);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE session_id='uuid-0' AND source='mcp' "
        "AND agent_id='bot-1' AND status='success' AND duration_ms=42"), 1);
    // per-tool_call trace_id was the next generated uuid (uuid-1).
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db_,
        "SELECT trace_id FROM agent_trace WHERE method='cortrix_query'", -1, &stmt, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))), "uuid-1");
    sqlite3_finalize(stmt);
}

TEST_F(McpSessionHandlerTest, FailedToolCallKeepsErrorOnly) {
    auto h = MakeHandler(1800);
    std::string sid = h->OnConnectionEstablished({});
    McpToolCall call{"upload", "{}"};
    McpToolResult res;
    res.is_success = false;
    res.error_code = "CX_ERR_NS_NOT_FOUND";
    res.error_message = "namespace sales does not exist";
    res.duration_ms = 7;
    EXPECT_TRUE(h->OnToolCall(sid, call, res));

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db_,
        "SELECT status, error_code, result_summary FROM agent_trace WHERE method='upload'",
        -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    auto col = [&](int i) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        return std::string(t ? t : "");
    };
    EXPECT_EQ(col(0), "failed");
    EXPECT_EQ(col(1), "CX_ERR_NS_NOT_FOUND");
    EXPECT_NE(col(2).find("CX_ERR_NS_NOT_FOUND"), std::string::npos);
    EXPECT_NE(col(2).find("namespace sales"), std::string::npos);
    sqlite3_finalize(stmt);
}

TEST_F(McpSessionHandlerTest, SessionEndOnClose) {
    auto h = MakeHandler(1800);
    std::string sid = h->OnConnectionEstablished({});
    EXPECT_EQ(AgentTraceMetrics::Instance().ActiveSessions(), 1);
    h->OnConnectionClosed(sid);
    EXPECT_EQ(h->active_session_count(), 0u);
    EXPECT_EQ(AgentTraceMetrics::Instance().ActiveSessions(), 0);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE method='session_end' AND status='success'"), 1);
    // Double-close is a no-op (no second session_end, gauge not driven negative).
    h->OnConnectionClosed(sid);
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE method='session_end'"), 1);
    EXPECT_EQ(AgentTraceMetrics::Instance().ActiveSessions(), 0);
}

TEST_F(McpSessionHandlerTest, IdleTimeoutWritesBackstopAndEvicts) {
    auto h = MakeHandler(/*idle=*/1800);  // 30 min
    std::string a = h->OnConnectionEstablished({});  // active at t=1_000_000
    clock_ms_.store(1'000'000 + 60'000);             // +1 min
    std::string b = h->OnConnectionEstablished({});  // active at t=1_060_000

    // Advance 31 min past 'a' (but only ~30 min past 'b').
    clock_ms_.store(1'000'000 + 31 * 60'000);
    int n = h->CheckIdleSessions();
    EXPECT_EQ(n, 1);  // only 'a' timed out
    EXPECT_EQ(h->active_session_count(), 1u);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE session_id='" + a +
        "' AND method='session_timeout'"), 1);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE session_id='" + b +
        "' AND method='session_timeout'"), 0);
    EXPECT_EQ(AgentTraceMetrics::Instance().ActiveSessions(), 1);
}

TEST_F(McpSessionHandlerTest, HardLimitDropsBeyond10K) {
    auto h = MakeHandler(1800);
    std::string sid = h->OnConnectionEstablished({});

    // Drive the counter to the limit without 10K real writes: reach in two phases
    // is impractical, so we assert the boundary via the public contract using a
    // small loop only up to a few, then verify the drop path by forcing the count.
    // Instead we exercise the real path with a modest count and check the metric
    // wiring on the boundary by calling exactly the limit+1 times is too slow;
    // here we validate the FIRST drop deterministically by lowering expectations:
    // perform kHardLimitToolCalls accepted calls, then one more must be dropped.
    McpToolCall call{"q", "{}"};
    McpToolResult res;
    res.is_success = true;
    for (int i = 0; i < McpSessionHandler::kHardLimitToolCalls; ++i) {
        ASSERT_TRUE(h->OnToolCall(sid, call, res)) << "accepted call " << i;
    }
    // The 10K-th accepted call crossed the long-session mark.
    EXPECT_EQ(AgentTraceMetrics::Instance().LongSessionCount(), 1u);
    // The next one is dropped.
    EXPECT_FALSE(h->OnToolCall(sid, call, res));
    EXPECT_EQ(AgentTraceMetrics::Instance().LongSessionDroppedCount(), 1u);
    // Exactly kHardLimitToolCalls rows were written (the dropped one was not).
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE method='q'"),
              McpSessionHandler::kHardLimitToolCalls);
}

TEST_F(McpSessionHandlerTest, TruncateParamsKeepsHeadAndTail) {
    std::string small = "small";
    EXPECT_EQ(McpSessionHandler::TruncateParams(small), small);

    std::string big(5000, 'x');
    std::string out = McpSessionHandler::TruncateParams(big);
    EXPECT_LT(out.size(), big.size());
    EXPECT_NE(out.find("[...truncated...]"), std::string::npos);
    EXPECT_EQ(out.substr(0, 10), std::string(10, 'x'));  // head preserved
}

TEST_F(McpSessionHandlerTest, TruncateResultAndFormatError) {
    EXPECT_EQ(McpSessionHandler::TruncateResult("ok"), "ok");
    std::string big(1000, 'y');
    std::string out = McpSessionHandler::TruncateResult(big);
    EXPECT_LT(out.size(), big.size());
    EXPECT_NE(out.find("[truncated]"), std::string::npos);

    // FormatError keeps code + first 256 chars.
    std::string longmsg(500, 'z');
    std::string fe = McpSessionHandler::FormatError(std::string("CX_ERR_X"), longmsg);
    EXPECT_EQ(fe.substr(0, 9), "CX_ERR_X:");
    EXPECT_LT(fe.size(), 9 + 500);  // message truncated to 256
    // null error_code falls back to CX_ERR_TRACE_INTERNAL.
    EXPECT_EQ(McpSessionHandler::FormatError(std::nullopt, "boom"),
              "CX_ERR_TRACE_INTERNAL: boom");
}

}  // namespace
}  // namespace cortrix::agent_trace
