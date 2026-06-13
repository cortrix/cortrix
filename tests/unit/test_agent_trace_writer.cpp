#include <gtest/gtest.h>

#include <type_traits>

#include <sqlite3.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/agent_trace_writer_impl.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/i_global_config.h"

// S3/S4/S10 coverage via the CE writer: Write() persists every call (incl.
// failures + session_end), Query() returns the filtered + paginated session with
// whole-session meta aggregates and CX_ERR_F13_INVALID_FILTER on bad input,
// CheckAndMarkTimeoutSessions() writes session_timeout backstops, Cleanup()
// deletes past the retention window.
namespace cortrix::agent_trace {
namespace {

// Minimal IGlobalConfig stub: only the POD retention fields matter here. The
// generic getters return InvalidArgument (unused by the writer).
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

class AgentTraceWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        config_ = std::make_shared<StubGlobalConfig>();
        writer_ = std::make_unique<AgentTraceWriterImpl>(db_, config_);
    }
    void TearDown() override {
        writer_.reset();
        if (db_) sqlite3_close(db_);
    }

    // Helper to make a trace entry with an explicit created_at (so tests control
    // ordering / age without sleeping).
    static AgentTraceEntry MakeEntry(const std::string& session, const std::string& method,
                                     const std::string& source, int64_t created_at,
                                     const std::string& status = "success") {
        AgentTraceEntry e;
        e.session_id = session;
        e.method = method;
        e.source = source;
        e.created_at = created_at;
        e.status = status;
        e.duration_ms = 10;
        return e;
    }

    AgentTraceSchemaProvider provider_;
    std::shared_ptr<StubGlobalConfig> config_;
    std::unique_ptr<AgentTraceWriterImpl> writer_;
    sqlite3* db_ = nullptr;
};

TEST_F(AgentTraceWriterTest, WritePersistsAllFieldsAndFillsCreatedAt) {
    AgentTraceEntry e;
    e.session_id = "sess-1";
    e.trace_id = "trace-1";
    e.agent_id = "agent-x";
    e.method = "cortrix_query";
    e.params = R"({"q":"hello"})";
    e.result_summary = "5 chunks";
    e.duration_ms = 145;
    e.source = "mcp";
    e.namespace_id = "sales";
    e.created_at = 0;  // writer fills now()
    writer_->Write(e);

    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE session_id='sess-1'"), 1);
    EXPECT_GT(QueryInt(db_, "SELECT created_at FROM agent_trace WHERE session_id='sess-1'"), 0);
    EXPECT_EQ(QueryInt(db_, "SELECT duration_ms FROM agent_trace WHERE session_id='sess-1'"), 145);
}

// Write() falls back to the W3C ctx trace_id when the entry leaves it unset (C6).
TEST_F(AgentTraceWriterTest, WriteFallsBackToCtxTraceId) {
    AgentTraceEntry e = MakeEntry("sess-ctx", "search", "http", 1000);
    // entry.trace_id unset.
    observability::TraceContext ctx;
    ctx.trace_id = "ctx-trace-99";
    writer_->Write(e, &ctx);

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db_,
        "SELECT trace_id FROM agent_trace WHERE session_id='sess-ctx'", -1, &stmt, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))),
              "ctx-trace-99");
    sqlite3_finalize(stmt);
}

// A failure row keeps status + error_code (topic 3 — even failed calls are traced).
TEST_F(AgentTraceWriterTest, WriteFailureRowKeepsErrorCode) {
    AgentTraceEntry e = MakeEntry("sess-f", "upload", "mcp", 1000, "failed");
    e.error_code = "CX_ERR_NS_NOT_FOUND";
    writer_->Write(e);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE status='failed' "
        "AND error_code='CX_ERR_NS_NOT_FOUND'"), 1);
}

TEST_F(AgentTraceWriterTest, QueryReturnsSessionWithMetaAggregates) {
    writer_->Write(MakeEntry("s", "a", "mcp", 1000));
    writer_->Write(MakeEntry("s", "b", "mcp", 2000));
    writer_->Write(MakeEntry("s", "c", "mcp", 3000));
    writer_->Write(MakeEntry("other", "x", "mcp", 5000));  // different session

    TraceFilter f;
    auto r = writer_->Query("s", f);
    ASSERT_TRUE(r.ok()) << r.status().message();
    const TraceSession& ts = r.value();
    EXPECT_EQ(ts.session_id, "s");
    EXPECT_EQ(ts.trace_count, 3);
    EXPECT_EQ(ts.total_count, 3);
    EXPECT_FALSE(ts.has_next);
    // meta over the whole session.
    EXPECT_EQ(ts.session_start, 1000);
    EXPECT_EQ(ts.session_end, 3000);
    EXPECT_EQ(ts.total_duration_ms, 30);  // 3 * 10ms
    // ascending call order.
    ASSERT_EQ(ts.traces.size(), 3u);
    EXPECT_EQ(ts.traces[0].method, "a");
    EXPECT_EQ(ts.traces[2].method, "c");
}

TEST_F(AgentTraceWriterTest, QueryPaginatesAndReportsHasNext) {
    for (int i = 0; i < 5; ++i)
        writer_->Write(MakeEntry("p", "m" + std::to_string(i), "http", 1000 + i));

    TraceFilter f;
    f.limit = 2;
    f.offset = 0;
    auto r = writer_->Query("p", f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().trace_count, 2);
    EXPECT_EQ(r.value().total_count, 5);
    EXPECT_TRUE(r.value().has_next);
    EXPECT_EQ(r.value().next_offset, 2);

    f.offset = 4;
    auto r2 = writer_->Query("p", f);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value().trace_count, 1);
    EXPECT_FALSE(r2.value().has_next);
}

TEST_F(AgentTraceWriterTest, QueryFilterByStatusAndAgent) {
    auto ok = MakeEntry("q", "a", "mcp", 1000, "success");
    ok.agent_id = "bot1";
    writer_->Write(ok);
    auto bad = MakeEntry("q", "b", "mcp", 2000, "failed");
    bad.agent_id = "bot2";
    writer_->Write(bad);

    TraceFilter f;
    f.status = "failed";
    auto r = writer_->Query("q", f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().trace_count, 1);
    EXPECT_EQ(r.value().traces[0].method, "b");

    TraceFilter f2;
    f2.agent_id = "bot1";
    auto r2 = writer_->Query("q", f2);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value().trace_count, 1);
    EXPECT_EQ(r2.value().traces[0].method, "a");
}

TEST_F(AgentTraceWriterTest, QueryInvalidFilterRejected) {
    TraceFilter bad_limit;
    bad_limit.limit = 0;
    auto r1 = writer_->Query("s", bad_limit);
    ASSERT_FALSE(r1.ok());
    EXPECT_NE(r1.status().message().find("CX_ERR_F13_INVALID_FILTER"), std::string::npos);

    TraceFilter big_limit;
    big_limit.limit = 999;
    EXPECT_FALSE(writer_->Query("s", big_limit).ok());

    TraceFilter bad_offset;
    bad_offset.offset = -1;
    EXPECT_FALSE(writer_->Query("s", bad_offset).ok());

    TraceFilter bad_range;
    bad_range.from_timestamp = 5000;
    bad_range.to_timestamp = 1000;
    auto r4 = writer_->Query("s", bad_range);
    ASSERT_FALSE(r4.ok());
    EXPECT_NE(r4.status().message().find("from_timestamp > to_timestamp"), std::string::npos);
}

TEST_F(AgentTraceWriterTest, QueryOffsetPastEndIsInvalid) {
    writer_->Write(MakeEntry("s", "a", "mcp", 1000));
    TraceFilter f;
    f.offset = 10;  // past the single row
    auto r = writer_->Query("s", f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F13_INVALID_FILTER"), std::string::npos);
}

TEST_F(AgentTraceWriterTest, QueryEmptySessionIsValidEmptyPage) {
    auto r = writer_->Query("nonexistent", TraceFilter{});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().trace_count, 0);
    EXPECT_EQ(r.value().total_count, 0);
    EXPECT_EQ(r.value().session_start, 0);
}

// CheckAndMarkTimeoutSessions writes a session_timeout row for an idle mcp session
// that never got a session_end, and leaves a closed / recent session alone.
TEST_F(AgentTraceWriterTest, CheckAndMarkTimeoutSessionsWritesBackstop) {
    const int64_t now = []{
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }();
    // idle session: last activity 1 hour ago, no terminal row.
    writer_->Write(MakeEntry("idle", "search", "mcp", now - 3600 * 1000));
    // closed session: has a session_end.
    writer_->Write(MakeEntry("closed", "search", "mcp", now - 3600 * 1000));
    writer_->Write(MakeEntry("closed", "session_end", "mcp", now - 3600 * 1000 + 1));
    // recent session: active 1 second ago.
    writer_->Write(MakeEntry("recent", "search", "mcp", now - 1000));

    // idle threshold = 30 min.
    writer_->CheckAndMarkTimeoutSessions(1800);

    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE session_id='idle' "
        "AND method='session_timeout'"), 1);
    // closed + recent get no new session_timeout row.
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE method='session_timeout'"), 1);
}

TEST_F(AgentTraceWriterTest, CleanupDeletesPastRetention) {
    const int64_t now = []{
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }();
    // 91 days old (past the 90-day default) + a fresh row.
    writer_->Write(MakeEntry("old", "a", "mcp", now - 91LL * 86400000LL));
    writer_->Write(MakeEntry("new", "b", "mcp", now - 1000));
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace"), 2);

    writer_->Cleanup();
    EXPECT_EQ(writer_->last_cleanup_deleted(), 1);
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace"), 1);
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE session_id='new'"), 1);
}

TEST_F(AgentTraceWriterTest, OnSessionEndIsNoThrowNoop) {
    // CE OnSessionEnd is a no-op (the session_end row is written via Write()); it
    // must not touch the DB or throw.
    writer_->Write(MakeEntry("s", "session_end", "mcp", 1000));
    EXPECT_NO_THROW(writer_->OnSessionEnd("s"));
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace"), 1);
}

// ---- additional branch coverage ----

// Write() classifies the metric WriteStatus from entry.status: the cancelled and
// session_timeout arms (plus the http source arm) are otherwise unexercised. Each
// still persists a row regardless of the metric label.
TEST_F(AgentTraceWriterTest, WriteCancelledStatusPersistsAndCounts) {
    writer_->Write(MakeEntry("sc", "search", "http", 1000, "cancelled"));
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE status='cancelled' AND source='http'"), 1);
}

TEST_F(AgentTraceWriterTest, WriteSessionTimeoutStatusPersists) {
    writer_->Write(MakeEntry("st", "session_timeout", "mcp", 1000, "session_timeout"));
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE status='session_timeout'"), 1);
}

// Query trace_id filter: the trace_id predicate is otherwise untested (status /
// agent_id are covered). Two rows, one matching trace_id.
TEST_F(AgentTraceWriterTest, QueryFilterByTraceId) {
    auto a = MakeEntry("qt", "m1", "mcp", 1000);
    a.trace_id = "tr-1";
    writer_->Write(a);
    auto b = MakeEntry("qt", "m2", "mcp", 2000);
    b.trace_id = "tr-2";
    writer_->Write(b);

    TraceFilter f;
    f.trace_id = "tr-2";
    auto r = writer_->Query("qt", f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().trace_count, 1);
    EXPECT_EQ(r.value().traces[0].method, "m2");
}

// Query namespace_id filter predicate (otherwise untested).
TEST_F(AgentTraceWriterTest, QueryFilterByNamespaceId) {
    auto a = MakeEntry("qn", "m1", "mcp", 1000);
    a.namespace_id = "sales";
    writer_->Write(a);
    auto b = MakeEntry("qn", "m2", "mcp", 2000);
    b.namespace_id = "eng";
    writer_->Write(b);

    TraceFilter f;
    f.namespace_id = "eng";
    auto r = writer_->Query("qn", f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().trace_count, 1);
    EXPECT_EQ(r.value().traces[0].method, "m2");
}

// Query timestamp-range predicates (created_at >= ? and created_at <= ?): a
// bounded window selects the middle row of three.
TEST_F(AgentTraceWriterTest, QueryFilterByTimestampWindow) {
    writer_->Write(MakeEntry("qw", "m1", "mcp", 1000));
    writer_->Write(MakeEntry("qw", "m2", "mcp", 2000));
    writer_->Write(MakeEntry("qw", "m3", "mcp", 3000));

    TraceFilter f;
    f.from_timestamp = 1500;
    f.to_timestamp = 2500;
    auto r = writer_->Query("qw", f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().trace_count, 1);
    EXPECT_EQ(r.value().traces[0].method, "m2");
}

// Cleanup with a null config falls back to the default 90-day retention (the
// config_ ? ... : 90 ternary's else arm). A row 91 days old is still purged.
TEST_F(AgentTraceWriterTest, CleanupWithNullConfigUsesDefaultRetention) {
    AgentTraceWriterImpl no_cfg(db_, /*config=*/nullptr);
    const int64_t now = []{
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }();
    no_cfg.Write(MakeEntry("oldn", "a", "mcp", now - 91LL * 86400000LL));
    no_cfg.Write(MakeEntry("freshn", "b", "mcp", now - 1000));
    no_cfg.Cleanup();
    EXPECT_EQ(no_cfg.last_cleanup_deleted(), 1);  // default 90d purged the old row
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE session_id='freshn'"), 1);
}

// CheckAndMarkTimeoutSessions ignores an idle HTTP session (the source='mcp'
// predicate is false) — only mcp sessions get the timeout backstop.
TEST_F(AgentTraceWriterTest, CheckAndMarkTimeoutIgnoresHttpSessions) {
    const int64_t now = []{
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }();
    writer_->Write(MakeEntry("http-idle", "search", "http", now - 3600 * 1000));
    writer_->CheckAndMarkTimeoutSessions(1800);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE method='session_timeout'"), 0);
}

// ---- storage-failure arms via a dropped table / abort trigger ------------------
//
// Query's count + page `prepare_v2 != SQLITE_OK` arms (CX_ERR_F13_INTERNAL) are
// reached by dropping agent_trace from the shared handle after a clean validate.
TEST_F(AgentTraceWriterTest, QueryCountPrepareFailureIsInternal) {
    writer_->Write(MakeEntry("s", "a", "mcp", 1000));
    ASSERT_EQ(sqlite3_exec(db_, "DROP TABLE agent_trace", nullptr, nullptr, nullptr),
              SQLITE_OK);
    auto r = writer_->Query("s", TraceFilter{});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F13_INTERNAL"), std::string::npos);
}

// Write's InsertLocked step-failure arm: a BEFORE INSERT abort trigger makes the
// prepared INSERT fail at step → last_error_ set, the failed-write metric counted.
TEST_F(AgentTraceWriterTest, WriteStepFailureRecordsLastError) {
    ASSERT_EQ(sqlite3_exec(db_,
        "CREATE TRIGGER t_block_at_ins BEFORE INSERT ON agent_trace "
        "BEGIN SELECT RAISE(ABORT,'blocked'); END;", nullptr, nullptr, nullptr),
        SQLITE_OK);
    writer_->Write(MakeEntry("sf", "a", "mcp", 1000));
    EXPECT_FALSE(writer_->last_error().empty());
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE session_id='sf'"), 0);
    sqlite3_exec(db_, "DROP TRIGGER t_block_at_ins", nullptr, nullptr, nullptr);
}

// CheckAndMarkTimeoutSessions' discovery-query prepare failure: dropping the table
// makes the GROUP BY/HAVING query fail to prepare → last_error_ set, early return.
TEST_F(AgentTraceWriterTest, CheckAndMarkTimeoutPrepareFailureRecordsError) {
    ASSERT_EQ(sqlite3_exec(db_, "DROP TABLE agent_trace", nullptr, nullptr, nullptr),
              SQLITE_OK);
    writer_->CheckAndMarkTimeoutSessions(1800);  // must not crash
    EXPECT_FALSE(writer_->last_error().empty());
}

// Cleanup's DELETE prepare-failure arm (the `== SQLITE_OK` guard is false) records
// last_error_ and leaves last_cleanup_deleted_ at 0.
TEST_F(AgentTraceWriterTest, CleanupPrepareFailureRecordsError) {
    ASSERT_EQ(sqlite3_exec(db_, "DROP TABLE agent_trace", nullptr, nullptr, nullptr),
              SQLITE_OK);
    writer_->Cleanup();
    EXPECT_EQ(writer_->last_cleanup_deleted(), 0);
    EXPECT_FALSE(writer_->last_error().empty());
}

// Cleanup's DELETE step-failure arm: prepares fine but a BEFORE DELETE abort
// trigger fails it at step (the `step == SQLITE_DONE` else arm).
TEST_F(AgentTraceWriterTest, CleanupDeleteStepFailureRecordsError) {
    const int64_t now = []{
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }();
    writer_->Write(MakeEntry("old", "a", "mcp", now - 200LL * 86400000LL));  // past 90d
    ASSERT_EQ(sqlite3_exec(db_,
        "CREATE TRIGGER t_block_at_del BEFORE DELETE ON agent_trace "
        "BEGIN SELECT RAISE(ABORT,'blocked'); END;", nullptr, nullptr, nullptr),
        SQLITE_OK);
    writer_->Cleanup();
    EXPECT_EQ(writer_->last_cleanup_deleted(), 0);
    EXPECT_FALSE(writer_->last_error().empty());
    sqlite3_exec(db_, "DROP TRIGGER t_block_at_del", nullptr, nullptr, nullptr);
}

// Query offset-zero on an empty filtered set is a valid empty page even when other
// rows exist for the session under a non-matching filter (the offset>0 guard false).
TEST_F(AgentTraceWriterTest, QueryFilteredEmptyOffsetZeroIsValid) {
    writer_->Write(MakeEntry("se", "a", "mcp", 1000));
    TraceFilter f;
    f.agent_id = "no-such-agent";  // matches nothing
    auto r = writer_->Query("se", f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total_count, 0);
    EXPECT_TRUE(r.value().traces.empty());
}

// The public interface stays abstract: embedders implement it against the
// frozen contract; CE injects AgentTraceWriterImpl via DI.
TEST(AgentTraceContractTest, InterfaceIsAbstract) {
    EXPECT_TRUE(std::is_abstract<IAgentTraceWriter>::value);
}

}  // namespace
}  // namespace cortrix::agent_trace
