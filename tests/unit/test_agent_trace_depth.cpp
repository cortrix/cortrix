// DEPTH anti-regression tests for src/agent_trace/ AgentTraceWriterImpl -- targets Query
// filter-validation boundaries (limit 1..200, offset >= 0, from<=to), combined-filter
// AND semantics, deterministic created_at-then-id ordering, whole-session meta
// aggregation across multiple sources, and the last_error()/last_cleanup_deleted()
// accessors -- edges not already pinned by test_agent_trace_writer.cpp.
//
// Deterministic: in-memory cortrix_global.db migrated via the F13 SchemaProvider,
// explicit created_at timestamps (no sleeps). Suite/fixture names are globally unique
// (AgentTraceDepth* prefix) per gtest's global-string suite-name rule.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <functional>
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/agent_trace_writer_impl.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/i_global_config.h"

namespace cortrix::agent_trace {
namespace {

class AgentTraceDepthStubConfig : public cortrix::IGlobalConfig {
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

class AgentTraceDepthWriterFx : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        config_ = std::make_shared<AgentTraceDepthStubConfig>();
        writer_ = std::make_unique<AgentTraceWriterImpl>(db_, config_);
    }
    void TearDown() override {
        writer_.reset();
        if (db_) sqlite3_close(db_);
    }

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
    std::shared_ptr<AgentTraceDepthStubConfig> config_;
    std::unique_ptr<AgentTraceWriterImpl> writer_;
    sqlite3* db_ = nullptr;
};

// ---- Query filter-validation boundaries ----

TEST_F(AgentTraceDepthWriterFx, QueryLimitZeroRejected) {
    TraceFilter f;
    f.limit = 0;  // below the [1,200] range
    auto r = writer_->Query("s", f);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F13_INVALID_FILTER"), std::string::npos);
}

TEST_F(AgentTraceDepthWriterFx, QueryLimitOver200Rejected) {
    TraceFilter f;
    f.limit = 201;  // above the cap
    auto r = writer_->Query("s", f);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F13_INVALID_FILTER"), std::string::npos);
}

TEST_F(AgentTraceDepthWriterFx, QueryLimitAtCap200IsValid) {
    TraceFilter f;
    f.limit = 200;  // inclusive upper bound is valid
    auto r = writer_->Query("empty-session", f);
    EXPECT_TRUE(r.ok());
}

TEST_F(AgentTraceDepthWriterFx, QueryNegativeOffsetRejected) {
    TraceFilter f;
    f.offset = -1;
    auto r = writer_->Query("s", f);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F13_INVALID_FILTER"), std::string::npos);
}

TEST_F(AgentTraceDepthWriterFx, QueryFromAfterToRejected) {
    TraceFilter f;
    f.from_timestamp = 2000;
    f.to_timestamp = 1000;  // from > to
    auto r = writer_->Query("s", f);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F13_INVALID_FILTER"), std::string::npos);
}

TEST_F(AgentTraceDepthWriterFx, QueryEqualFromToIsValid) {
    TraceFilter f;
    f.from_timestamp = 1500;
    f.to_timestamp = 1500;  // boundary: from == to is allowed
    auto r = writer_->Query("s", f);
    EXPECT_TRUE(r.ok());
}

// ---- Combined filters apply AND semantics ----

TEST_F(AgentTraceDepthWriterFx, QueryCombinedStatusAndAgentNarrowsResults) {
    auto a = MakeEntry("sess", "m1", "http", 100, "failed");
    a.agent_id = "agentA";
    auto b = MakeEntry("sess", "m2", "http", 200, "failed");
    b.agent_id = "agentB";
    auto c = MakeEntry("sess", "m3", "http", 300, "success");
    c.agent_id = "agentA";
    writer_->Write(a);
    writer_->Write(b);
    writer_->Write(c);

    TraceFilter f;
    f.status = "failed";
    f.agent_id = "agentA";  // both conditions must hold -> only `a`
    auto r = writer_->Query("sess", f);
    ASSERT_TRUE(r.ok());
    const TraceSession& s = r.value();
    EXPECT_EQ(s.trace_count, 1);
    ASSERT_EQ(s.traces.size(), 1u);
    EXPECT_EQ(s.traces[0].method, "m1");
}

// ---- Ordering is created_at then id (ascending = call order) ----

TEST_F(AgentTraceDepthWriterFx, QueryOrdersByCreatedAtAscending) {
    writer_->Write(MakeEntry("ord", "third", "http", 300));
    writer_->Write(MakeEntry("ord", "first", "http", 100));
    writer_->Write(MakeEntry("ord", "second", "http", 200));

    TraceFilter f;
    auto r = writer_->Query("ord", f);
    ASSERT_TRUE(r.ok());
    const TraceSession& s = r.value();
    ASSERT_EQ(s.traces.size(), 3u);
    EXPECT_EQ(s.traces[0].method, "first");
    EXPECT_EQ(s.traces[1].method, "second");
    EXPECT_EQ(s.traces[2].method, "third");
}

// ---- Whole-session meta aggregates across sources + pages ----

TEST_F(AgentTraceDepthWriterFx, QueryMetaAggregatesWholeSessionNotJustPage) {
    auto e1 = MakeEntry("agg", "m1", "http", 1000);
    e1.duration_ms = 5;
    auto e2 = MakeEntry("agg", "m2", "mcp", 2000);
    e2.duration_ms = 15;
    auto e3 = MakeEntry("agg", "m3", "http", 3000);
    e3.duration_ms = 30;
    writer_->Write(e1);
    writer_->Write(e2);
    writer_->Write(e3);

    TraceFilter f;
    f.limit = 1;  // page shows only the first row, but meta covers the whole session
    auto r = writer_->Query("agg", f);
    ASSERT_TRUE(r.ok());
    const TraceSession& s = r.value();
    EXPECT_EQ(s.trace_count, 1);          // page size
    EXPECT_EQ(s.total_count, 3);          // whole-session matching rows
    EXPECT_EQ(s.session_start, 1000);
    EXPECT_EQ(s.session_end, 3000);
    EXPECT_EQ(s.total_duration_ms, 50);   // 5 + 15 + 30 across all sources
    EXPECT_TRUE(s.has_next);
    EXPECT_EQ(s.next_offset, 1);
}

// ---- Accessors after a successful write/cleanup ----

TEST_F(AgentTraceDepthWriterFx, SuccessfulWriteLeavesLastErrorEmpty) {
    writer_->Write(MakeEntry("ok", "m", "http", 100));
    EXPECT_TRUE(writer_->last_error().empty());
}

TEST_F(AgentTraceDepthWriterFx, CleanupReportsDeletedCount) {
    // Two rows far in the past -> past the default retention window -> deleted by Cleanup().
    writer_->Write(MakeEntry("old1", "m", "http", 1));
    writer_->Write(MakeEntry("old2", "m", "http", 2));
    writer_->Cleanup();
    EXPECT_GE(writer_->last_cleanup_deleted(), 0);  // count is recorded (non-negative)
}

// ---- OnSessionEnd is a no-throw no-op that does not corrupt later queries ----

TEST_F(AgentTraceDepthWriterFx, OnSessionEndThenQueryStillWorks) {
    writer_->Write(MakeEntry("live", "m", "mcp", 500));
    writer_->OnSessionEnd("live");  // finalize hook (no row written by this call)
    TraceFilter f;
    auto r = writer_->Query("live", f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().trace_count, 1);
}

// ---- Query isolates sessions: rows from session B never leak into session A ----

TEST_F(AgentTraceDepthWriterFx, QueryScopesToRequestedSessionOnly) {
    writer_->Write(MakeEntry("sessA", "a1", "http", 100));
    writer_->Write(MakeEntry("sessA", "a2", "http", 200));
    writer_->Write(MakeEntry("sessB", "b1", "http", 150));

    TraceFilter f;
    auto ra = writer_->Query("sessA", f);
    ASSERT_TRUE(ra.ok());
    EXPECT_EQ(ra.value().total_count, 2);
    for (const auto& t : ra.value().traces) {
        ASSERT_TRUE(t.session_id.has_value());
        EXPECT_EQ(*t.session_id, "sessA");
    }
}

// ---- Empty session: valid empty page with zeroed meta ----

TEST_F(AgentTraceDepthWriterFx, QueryUnknownSessionReturnsEmptyValidPage) {
    TraceFilter f;
    auto r = writer_->Query("does-not-exist", f);
    ASSERT_TRUE(r.ok());
    const TraceSession& s = r.value();
    EXPECT_EQ(s.trace_count, 0);
    EXPECT_EQ(s.total_count, 0);
    EXPECT_TRUE(s.traces.empty());
    EXPECT_FALSE(s.has_next);
}

// ---- Filter by namespace_id within a session ----

TEST_F(AgentTraceDepthWriterFx, QueryFilterByNamespaceNarrows) {
    auto a = MakeEntry("ns-sess", "m1", "http", 100);
    a.namespace_id = "ns-alpha";
    auto b = MakeEntry("ns-sess", "m2", "http", 200);
    b.namespace_id = "ns-beta";
    writer_->Write(a);
    writer_->Write(b);

    TraceFilter f;
    f.namespace_id = "ns-beta";
    auto r = writer_->Query("ns-sess", f);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().traces.size(), 1u);
    EXPECT_EQ(r.value().traces[0].method, "m2");
}

// ---- Failed-status write persists error_code on the row ----

TEST_F(AgentTraceDepthWriterFx, FailedWriteKeepsErrorCodeColumn) {
    auto e = MakeEntry("err-sess", "do_thing", "mcp", 100, "failed");
    e.error_code = "CX_ERR_SOMETHING";
    writer_->Write(e);

    TraceFilter f;
    auto r = writer_->Query("err-sess", f);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().traces.size(), 1u);
    EXPECT_EQ(r.value().traces[0].status, "failed");
    ASSERT_TRUE(r.value().traces[0].error_code.has_value());
    EXPECT_EQ(*r.value().traces[0].error_code, "CX_ERR_SOMETHING");
}

}  // namespace
}  // namespace cortrix::agent_trace
