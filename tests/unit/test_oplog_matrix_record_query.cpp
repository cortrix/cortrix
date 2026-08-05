// Operation-log + observability record/query MATRICES (BREADTH).
//
// Surfaces (all deterministic: in-memory SQLite, fixed timestamps, no clock/IO):
//   A. OperationLogger record (Log/BatchLog) x query (filter-dimension x
//      pagination x sort) matrices - the user-view track (operation log).
//   B. observability operation_log SCHEMA / version / migrate projections (operation log).
//   C. ObservabilityContext / ObservabilityValidator identity-header + MCP
//      capability projections, FormatStructured trace correlation (agent trace/sec 6.1).
//   D. AgentTraceWriterImpl record/query EDGES not covered by the metrics matrix
//      (the ops-view track, agent trace).
//
// Does NOT duplicate the OperationLoggerTest cleanup/metric-emit sweeps or the
// agent_trace metrics_matrix label sweeps - this is the record/query + schema +
// context-projection angle. Prefixes are globally unique: OpLogMatrix*.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/agent_trace_writer_impl.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/observability/observability_context.h"
#include "cortrix/observability/operation_log_schema.h"
#include "cortrix/observability/operation_logger_impl.h"
#include "cortrix/observability/trace_context.h"

namespace cortrix {
namespace {

// =====================================================================
// A. OperationLogger record x query matrices
// =====================================================================

class OpLogMatrixLoggerFx : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        config_ = std::make_shared<InMemoryGlobalConfig>();
        logger_ = std::make_unique<observability::OperationLogger>(db_, config_);
    }
    void TearDown() override {
        logger_.reset();
        if (db_) sqlite3_close(db_);
    }

    observability::OperationLogEntry E(const std::string& user, const std::string& action,
                                       const std::string& rtype, int64_t ts) {
        observability::OperationLogEntry e;
        e.timestamp = ts;
        e.user_id = user;
        e.action = action;
        e.resource_type = rtype;
        return e;
    }

    observability::OperationLogSchemaProvider provider_;
    sqlite3* db_ = nullptr;
    std::shared_ptr<InMemoryGlobalConfig> config_;
    std::unique_ptr<observability::OperationLogger> logger_;
};

// --- record matrix: action x resource_type combos all persist & round-trip ---
struct RecRow { const char* action; const char* rtype; };
class OpLogMatrixRecord
    : public OpLogMatrixLoggerFx,
      public ::testing::WithParamInterface<RecRow> {};

TEST_P(OpLogMatrixRecord, ActionResourceRoundTrips) {
    const RecRow& r = GetParam();
    logger_->Log(E("alice", r.action, r.rtype, 1000));
    observability::OperationLogFilter f;
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok()) << res.status().message();
    ASSERT_EQ(res->entries.size(), 1u);
    EXPECT_EQ(res->entries[0].action, r.action);
    EXPECT_EQ(res->entries[0].resource_type, r.rtype);
}
INSTANTIATE_TEST_SUITE_P(
    Combos, OpLogMatrixRecord,
    ::testing::Values(RecRow{"document_upload", "document"},
                      RecRow{"document_delete", "document"},
                      RecRow{"namespace_create", "namespace"},
                      RecRow{"namespace_delete", "namespace"},
                      RecRow{"memory_create", "memory"},
                      RecRow{"memory_edit", "memory"},
                      RecRow{"memory_delete", "memory"},
                      RecRow{"query_run", "query"},
                      RecRow{"db_connection_add", "db_connection"},
                      RecRow{"db_import_start", "db_import"},
                      RecRow{"document_download", "document"},
                      RecRow{"namespace_update", "namespace"},
                      RecRow{"memory_recall", "memory"},
                      RecRow{"query_explain", "query"},
                      RecRow{"db_connection_remove", "db_connection"},
                      RecRow{"db_import_finish", "db_import"}));

// --- single-action filter matrix: filtering on one action isolates exactly it ---
class OpLogMatrixActionFilter
    : public OpLogMatrixLoggerFx,
      public ::testing::WithParamInterface<const char*> {};

TEST_P(OpLogMatrixActionFilter, FilterIsolatesAction) {
    logger_->Log(E("u", "document_upload", "document", 1000));
    logger_->Log(E("u", "memory_create", "memory", 2000));
    logger_->Log(E("u", "query_run", "query", 3000));
    observability::OperationLogFilter f;
    f.action = GetParam();
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 1);
    if (res->entries.size() == 1u)
        EXPECT_EQ(res->entries[0].action, GetParam());
}
INSTANTIATE_TEST_SUITE_P(
    Each, OpLogMatrixActionFilter,
    ::testing::Values("document_upload", "memory_create", "query_run"));

// A filter action that matches no row returns an empty (but OK) result.
TEST_F(OpLogMatrixLoggerFx, FilterNoMatchEmptyOk) {
    logger_->Log(E("u", "document_upload", "document", 1000));
    observability::OperationLogFilter f;
    f.action = "nonexistent_action";
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 0);
    EXPECT_TRUE(res->entries.empty());
}

// --- pagination x sort matrix over a fixed 6-row corpus ---
struct PageRow { int limit; int offset; const char* sort; int expect_size; bool has_next; };
class OpLogMatrixPagination
    : public OpLogMatrixLoggerFx,
      public ::testing::WithParamInterface<PageRow> {};

TEST_P(OpLogMatrixPagination, PageWindowAndHasNext) {
    for (int i = 1; i <= 6; ++i) logger_->Log(E("u", "query_run", "query", i * 100));
    const PageRow& p = GetParam();
    observability::OperationLogFilter f;
    f.limit = p.limit;
    f.offset = p.offset;
    f.sort_order = p.sort;
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok()) << res.status().message();
    EXPECT_EQ(res->total_count, 6);
    EXPECT_EQ(static_cast<int>(res->entries.size()), p.expect_size);
    EXPECT_EQ(res->has_next, p.has_next);
}
INSTANTIATE_TEST_SUITE_P(
    Windows, OpLogMatrixPagination,
    ::testing::Values(PageRow{2, 0, "DESC", 2, true},
                      PageRow{2, 2, "DESC", 2, true},
                      PageRow{2, 4, "DESC", 2, false},
                      PageRow{3, 0, "ASC", 3, true},
                      PageRow{3, 3, "ASC", 3, false},
                      PageRow{6, 0, "DESC", 6, false},
                      PageRow{10, 0, "ASC", 6, false},
                      PageRow{5, 0, "DESC", 5, true},
                      PageRow{1, 0, "ASC", 1, true},
                      PageRow{1, 5, "ASC", 1, false},
                      PageRow{4, 0, "DESC", 4, true},
                      PageRow{4, 4, "DESC", 2, false},
                      PageRow{3, 3, "DESC", 3, false}));

// --- sort order projects newest/oldest first correctly ---
TEST_F(OpLogMatrixLoggerFx, SortDescNewestFirst) {
    logger_->Log(E("u", "query_run", "query", 100));
    logger_->Log(E("u", "query_run", "query", 300));
    logger_->Log(E("u", "query_run", "query", 200));
    observability::OperationLogFilter f;
    f.sort_order = "DESC";
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->entries.size(), 3u);
    EXPECT_EQ(res->entries[0].timestamp, 300);
    EXPECT_EQ(res->entries[2].timestamp, 100);
}
TEST_F(OpLogMatrixLoggerFx, SortAscOldestFirst) {
    logger_->Log(E("u", "query_run", "query", 100));
    logger_->Log(E("u", "query_run", "query", 300));
    logger_->Log(E("u", "query_run", "query", 200));
    observability::OperationLogFilter f;
    f.sort_order = "ASC";
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->entries.size(), 3u);
    EXPECT_EQ(res->entries[0].timestamp, 100);
    EXPECT_EQ(res->entries[2].timestamp, 300);
}

// --- multi-dimension filter matrix (combinations of user/ns/rtype/action) ---
TEST_F(OpLogMatrixLoggerFx, FilterUserAndNamespace) {
    observability::OperationLogEntry a = E("alice", "memory_create", "memory", 1000);
    a.namespace_id = "sales";
    logger_->Log(a);
    observability::OperationLogEntry b = E("bob", "memory_create", "memory", 2000);
    b.namespace_id = "sales";
    logger_->Log(b);
    observability::OperationLogEntry c = E("alice", "memory_create", "memory", 3000);
    c.namespace_id = "eng";
    logger_->Log(c);

    observability::OperationLogFilter f;
    f.user_id = "alice";
    f.namespace_id = "sales";
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 1);
}

TEST_F(OpLogMatrixLoggerFx, FilterActionInPlusResourceType) {
    logger_->Log(E("u", "memory_create", "memory", 1000));
    logger_->Log(E("u", "memory_delete", "memory", 2000));
    logger_->Log(E("u", "document_upload", "document", 3000));

    observability::OperationLogFilter f;
    f.action_in = std::vector<std::string>{"memory_create", "memory_delete", "document_upload"};
    f.resource_type = "memory";
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 2);
}

// --- timestamp range matrix (inclusive bounds) ---
struct RangeRow { int64_t from; int64_t to; int expect; };
class OpLogMatrixTimeRange
    : public OpLogMatrixLoggerFx,
      public ::testing::WithParamInterface<RangeRow> {};
TEST_P(OpLogMatrixTimeRange, RangeFiltersCount) {
    for (int i = 1; i <= 5; ++i) logger_->Log(E("u", "query_run", "query", i * 1000));
    const RangeRow& r = GetParam();
    observability::OperationLogFilter f;
    f.from_timestamp = r.from;
    f.to_timestamp = r.to;
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, r.expect);
}
INSTANTIATE_TEST_SUITE_P(
    Ranges, OpLogMatrixTimeRange,
    ::testing::Values(RangeRow{1000, 5000, 5},
                      RangeRow{2000, 4000, 3},
                      RangeRow{2500, 3500, 1},
                      RangeRow{6000, 9000, 0},
                      RangeRow{1, 1500, 1},
                      RangeRow{1000, 1000, 1},
                      RangeRow{3000, 5000, 3},
                      RangeRow{1500, 4500, 3}));

// --- BatchLog: all-or-nothing commit count matrix ---
struct BatchRow { int n; };
class OpLogMatrixBatch
    : public OpLogMatrixLoggerFx,
      public ::testing::WithParamInterface<BatchRow> {};
TEST_P(OpLogMatrixBatch, BatchCommitsAllRows) {
    std::vector<observability::OperationLogEntry> batch;
    for (int i = 0; i < GetParam().n; ++i)
        batch.push_back(E("u", "query_run", "query", 1000 + i));
    logger_->BatchLog(batch);
    observability::OperationLogFilter f;
    auto res = logger_->Query(f);
    if (GetParam().n == 0) {
        // empty page is a valid OK result with 0 rows
        ASSERT_TRUE(res.ok());
        EXPECT_EQ(res->total_count, 0);
    } else {
        ASSERT_TRUE(res.ok());
        EXPECT_EQ(res->total_count, GetParam().n);
    }
}
INSTANTIATE_TEST_SUITE_P(
    Sizes, OpLogMatrixBatch,
    ::testing::Values(BatchRow{0}, BatchRow{1}, BatchRow{3}, BatchRow{10},
                      BatchRow{2}, BatchRow{5}, BatchRow{20}, BatchRow{50}));

// --- invalid-filter rejection matrix -> CX_ERR_OPLOG_* (record/query guard) ---
struct BadFilterRow { int limit; int offset; const char* sort; const char* token; };
class OpLogMatrixBadFilter
    : public OpLogMatrixLoggerFx,
      public ::testing::WithParamInterface<BadFilterRow> {};
TEST_P(OpLogMatrixBadFilter, RejectedWithToken) {
    logger_->Log(E("u", "query_run", "query", 1000));
    const BadFilterRow& r = GetParam();
    observability::OperationLogFilter f;
    f.limit = r.limit;
    f.offset = r.offset;
    f.sort_order = r.sort;
    auto res = logger_->Query(f);
    ASSERT_FALSE(res.ok());
    EXPECT_NE(res.status().message().find(r.token), std::string::npos)
        << res.status().message();
}
INSTANTIATE_TEST_SUITE_P(
    Guards, OpLogMatrixBadFilter,
    ::testing::Values(BadFilterRow{0, 0, "DESC", "CX_ERR_OPLOG_INVALID_FILTER"},
                      BadFilterRow{201, 0, "DESC", "CX_ERR_OPLOG_INVALID_FILTER"},
                      BadFilterRow{50, -1, "DESC", "CX_ERR_OPLOG_INVALID_FILTER"},
                      BadFilterRow{50, 0, "SIDEWAYS", "CX_ERR_OPLOG_INVALID_FILTER"}));

// =====================================================================
// B. operation_log schema / version / migrate projections
// =====================================================================

class OpLogMatrixSchemaFx : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { if (db_) sqlite3_close(db_); }
    bool HasTable(const char* name) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1, &s, nullptr);
        sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
        bool found = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
        return found;
    }
    int IndexCount(const char* tbl) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND tbl_name=?1 "
            "AND sql IS NOT NULL", -1, &s, nullptr);
        sqlite3_bind_text(s, 1, tbl, -1, SQLITE_TRANSIENT);
        int n = 0;
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        return n;
    }
    sqlite3* db_ = nullptr;
};

TEST_F(OpLogMatrixSchemaFx, VersionConstantIsOne) {
    EXPECT_EQ(observability::kOplogSchemaVersion, 1);
    observability::OperationLogSchemaProvider p;
    EXPECT_EQ(p.CurrentVersion(), 1);
    EXPECT_EQ(p.FeatureName(), "F18a");
}

TEST_F(OpLogMatrixSchemaFx, MigrateZeroToOneCreatesTableAndIndices) {
    observability::OperationLogSchemaProvider p;
    auto st = p.Migrate(db_, 0, 1);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(HasTable("operation_log"));
    EXPECT_EQ(IndexCount("operation_log"), 5);  // sec 5.1 = 5 indices
}

TEST_F(OpLogMatrixSchemaFx, MigrateOneToOneIsDefensiveNoOp) {
    observability::OperationLogSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
    auto st = p.Migrate(db_, 1, 1);  // already current -> no-op
    EXPECT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(HasTable("operation_log"));
}

TEST_F(OpLogMatrixSchemaFx, MigrateViaSharedMigratorSucceeds) {
    observability::OperationLogSchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    auto st = m.MigrateCatalog(db_);
    EXPECT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(HasTable("operation_log"));
}

// agent_trace schema parallel projection (agent trace = 3 indices).
TEST_F(OpLogMatrixSchemaFx, AgentTraceSchemaVersionAndProvider) {
    EXPECT_EQ(agent_trace::kAgentTraceSchemaVersion, 1);
    agent_trace::AgentTraceSchemaProvider p;
    EXPECT_EQ(p.CurrentVersion(), 1);
    EXPECT_EQ(p.FeatureName(), "F13");
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
    EXPECT_TRUE(HasTable("agent_trace"));
    EXPECT_EQ(IndexCount("agent_trace"), 3);
}

// =====================================================================
// C. ObservabilityValidator / ObservabilityContext projections
// =====================================================================

using observability::ObservabilityValidator;

// Identity-format whitelist matrix (agent trace [a-zA-Z0-9_.:/-], <=128).
struct IdRow { const char* value; bool valid; };
class OpLogMatrixIdFormat : public ::testing::TestWithParam<IdRow> {};
TEST_P(OpLogMatrixIdFormat, IsValidFormatMatrix) {
    const IdRow& r = GetParam();
    EXPECT_EQ(ObservabilityValidator::IsValidFormat(
                  r.value, ObservabilityValidator::kMaxIdentityLength),
              r.valid)
        << "value=" << r.value;
}
INSTANTIATE_TEST_SUITE_P(
    Formats, OpLogMatrixIdFormat,
    ::testing::Values(IdRow{"sess_123", true},
                      IdRow{"trace.id:abc/def-1", true},
                      IdRow{"ABCxyz_0", true},
                      IdRow{"", false},               // empty
                      IdRow{"has space", false},      // space not in whitelist
                      IdRow{"semi;colon", false},     // ';' not allowed
                      IdRow{"new\nline", false},      // control char
                      IdRow{"comma,sep", false},      // ',' not allowed
                      IdRow{"a", true},               // single char
                      IdRow{"123456789", true},       // digits only
                      IdRow{"path/to/x", true},       // slash allowed
                      IdRow{"a.b.c", true},           // dots allowed
                      IdRow{"x:y:z", true},           // colons allowed
                      IdRow{"has\ttab", false},       // tab not allowed
                      IdRow{"paren(s)", false},       // parens not allowed
                      IdRow{"at@sign", false}));      // '@' not allowed

// Over-length is rejected (kMaxIdentityLength + 1 chars).
TEST(OpLogMatrixIdLength, OverMaxRejected) {
    std::string s(ObservabilityValidator::kMaxIdentityLength + 1, 'a');
    EXPECT_FALSE(ObservabilityValidator::IsValidFormat(
        s, ObservabilityValidator::kMaxIdentityLength));
    std::string ok(ObservabilityValidator::kMaxIdentityLength, 'a');
    EXPECT_TRUE(ObservabilityValidator::IsValidFormat(
        ok, ObservabilityValidator::kMaxIdentityLength));
}

// Validate{Session,Trace,Agent}Id Result projection over valid/invalid.
TEST(OpLogMatrixValidate, ValidIdsReturnOkValue) {
    auto s = ObservabilityValidator::ValidateSessionId("sess-1");
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value(), "sess-1");
    auto t = ObservabilityValidator::ValidateTraceId("0af7651916cd43dd");
    ASSERT_TRUE(t.ok());
    auto a = ObservabilityValidator::ValidateAgentId("agent.x");
    ASSERT_TRUE(a.ok());
}
TEST(OpLogMatrixValidate, InvalidIdsReturnInvalidArgument) {
    auto s = ObservabilityValidator::ValidateSessionId("bad space");
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.status().message().find("CX_ERR_F13_INVALID_FILTER"), std::string::npos);
}

// FromHttpHeaders: valid headers populate identity; invalid ones are dropped.
TEST(OpLogMatrixContext, FromHttpHeadersValidPopulates) {
    observability::HttpHeaders h;
    h.values["X-Session-Id"] = "sess-9";
    h.values["X-Agent-Id"] = "agent-1";
    auto ctx = observability::ObservabilityContext::FromHttpHeaders(h);
    EXPECT_EQ(ctx.session_id, std::optional<std::string>("sess-9"));
    EXPECT_EQ(ctx.agent_id, std::optional<std::string>("agent-1"));
    EXPECT_GT(ctx.created_at, 0);
}
TEST(OpLogMatrixContext, FromHttpHeadersInvalidDropped) {
    observability::HttpHeaders h;
    h.values["X-Session-Id"] = "bad space";  // invalid -> dropped (left unset)
    auto ctx = observability::ObservabilityContext::FromHttpHeaders(h);
    EXPECT_FALSE(ctx.session_id.has_value());
}
TEST(OpLogMatrixContext, FromHttpHeadersEmptyLeavesUnset) {
    observability::HttpHeaders h;
    auto ctx = observability::ObservabilityContext::FromHttpHeaders(h);
    EXPECT_FALSE(ctx.session_id.has_value());
    EXPECT_FALSE(ctx.agent_id.has_value());
}

// FromMcpCapability: session_id is taken as-is; agent_id / namespace_id pass through.
TEST(OpLogMatrixContext, FromMcpCapabilityProjectsFields) {
    observability::McpSession s;
    s.session_id = "mcp-sess";
    s.agent_id = "mcp-agent";
    s.namespace_id = "sales";
    auto ctx = observability::ObservabilityContext::FromMcpCapability(s);
    EXPECT_EQ(ctx.session_id, std::optional<std::string>("mcp-sess"));
    EXPECT_EQ(ctx.agent_id, std::optional<std::string>("mcp-agent"));
    EXPECT_EQ(ctx.namespace_id, std::optional<std::string>("sales"));
    EXPECT_GT(ctx.created_at, 0);
}

// FormatStructured: trace_id injected when a TraceContext is set; null otherwise.
TEST(OpLogMatrixContext, FormatStructuredInjectsTraceId) {
    observability::ObservabilityContext ctx;
    observability::TraceContext tc;
    tc.trace_id = "abc123";
    tc.span_id = "span9";
    ctx.SetTraceContext(tc);
    std::string line = ctx.FormatStructured(observability::LogLevel::kInfo, "hello");
    EXPECT_NE(line.find("abc123"), std::string::npos);
    EXPECT_NE(line.find("hello"), std::string::npos);
}
TEST(OpLogMatrixContext, FormatStructuredNoTraceStillEmits) {
    observability::ObservabilityContext ctx;  // no trace context
    std::string line = ctx.FormatStructured(observability::LogLevel::kError, "boom");
    EXPECT_NE(line.find("boom"), std::string::npos);
}

// LogLevel ToString matrix.
struct LvlRow { observability::LogLevel lvl; const char* str; };
class OpLogMatrixLogLevel : public ::testing::TestWithParam<LvlRow> {};
TEST_P(OpLogMatrixLogLevel, ToStringMatrix) {
    EXPECT_STREQ(observability::ToString(GetParam().lvl), GetParam().str);
}
INSTANTIATE_TEST_SUITE_P(
    Levels, OpLogMatrixLogLevel,
    ::testing::Values(LvlRow{observability::LogLevel::kDebug, "debug"},
                      LvlRow{observability::LogLevel::kInfo, "info"},
                      LvlRow{observability::LogLevel::kWarn, "warn"},
                      LvlRow{observability::LogLevel::kError, "error"}));

// =====================================================================
// D. AgentTraceWriterImpl record/query edges (ops-view track)
// =====================================================================

class OpLogMatrixTraceFx : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        config_ = std::make_shared<InMemoryGlobalConfig>();
        writer_ = std::make_unique<agent_trace::AgentTraceWriterImpl>(db_, config_);
    }
    void TearDown() override {
        writer_.reset();
        if (db_) sqlite3_close(db_);
    }
    agent_trace::AgentTraceEntry T(const std::string& sess, const std::string& method,
                                   const std::string& status, int64_t at) {
        agent_trace::AgentTraceEntry e;
        e.session_id = sess;
        e.method = method;
        e.status = status;
        e.source = "http";
        e.created_at = at;
        return e;
    }
    agent_trace::AgentTraceSchemaProvider provider_;
    sqlite3* db_ = nullptr;
    std::shared_ptr<InMemoryGlobalConfig> config_;
    std::unique_ptr<agent_trace::AgentTraceWriterImpl> writer_;
};

// Write persists a row; Query returns it with whole-session meta aggregates.
TEST_F(OpLogMatrixTraceFx, WriteThenQuerySessionMeta) {
    agent_trace::AgentTraceEntry a = T("s1", "search", "success", 1000);
    a.duration_ms = 30;
    writer_->Write(a);
    agent_trace::AgentTraceEntry b = T("s1", "fetch", "success", 2000);
    b.duration_ms = 70;
    writer_->Write(b);

    agent_trace::TraceFilter f;
    auto res = writer_->Query("s1", f);
    ASSERT_TRUE(res.ok()) << res.status().message();
    EXPECT_EQ(res->session_id, "s1");
    EXPECT_EQ(res->trace_count, 2);
    EXPECT_EQ(res->session_start, 1000);
    EXPECT_EQ(res->session_end, 2000);
    EXPECT_EQ(res->total_duration_ms, 100);
    EXPECT_EQ(res->total_count, 2);
}

// status filter matrix: each status value isolates its rows.
struct StatusRow { const char* status; };
class OpLogMatrixTraceStatus
    : public OpLogMatrixTraceFx,
      public ::testing::WithParamInterface<StatusRow> {};
TEST_P(OpLogMatrixTraceStatus, StatusFilterIsolates) {
    writer_->Write(T("s1", "m", "success", 1000));
    writer_->Write(T("s1", "m", "failed", 2000));
    writer_->Write(T("s1", "m", "cancelled", 3000));
    writer_->Write(T("s1", "m", "session_timeout", 4000));
    agent_trace::TraceFilter f;
    f.status = GetParam().status;
    auto res = writer_->Query("s1", f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 1);
    if (!res->traces.empty()) EXPECT_EQ(res->traces[0].status, GetParam().status);
}
INSTANTIATE_TEST_SUITE_P(
    States, OpLogMatrixTraceStatus,
    ::testing::Values(StatusRow{"success"}, StatusRow{"failed"},
                      StatusRow{"cancelled"}, StatusRow{"session_timeout"}));

// Failure rows carry error_code and round-trip it.
TEST_F(OpLogMatrixTraceFx, FailureRowCarriesErrorCode) {
    agent_trace::AgentTraceEntry e = T("s2", "search", "failed", 1000);
    e.error_code = "CX_ERR_NS_TIMEOUT";
    e.result_summary = "timed out";
    writer_->Write(e);
    agent_trace::TraceFilter f;
    auto res = writer_->Query("s2", f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->traces.size(), 1u);
    EXPECT_EQ(res->traces[0].error_code, std::optional<std::string>("CX_ERR_NS_TIMEOUT"));
}

// trace_id / agent_id filter dimensions.
TEST_F(OpLogMatrixTraceFx, FilterByTraceIdAndAgentId) {
    agent_trace::AgentTraceEntry a = T("s3", "m", "success", 1000);
    a.trace_id = "T1";
    a.agent_id = "A1";
    writer_->Write(a);
    agent_trace::AgentTraceEntry b = T("s3", "m", "success", 2000);
    b.trace_id = "T2";
    b.agent_id = "A2";
    writer_->Write(b);

    agent_trace::TraceFilter ft;
    ft.trace_id = "T1";
    auto r1 = writer_->Query("s3", ft);
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1->total_count, 1);

    agent_trace::TraceFilter fa;
    fa.agent_id = "A2";
    auto r2 = writer_->Query("s3", fa);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2->total_count, 1);
}

// Pagination matrix on a session's traces.
struct TPageRow { int limit; int offset; int expect; bool has_next; };
class OpLogMatrixTracePage
    : public OpLogMatrixTraceFx,
      public ::testing::WithParamInterface<TPageRow> {};
TEST_P(OpLogMatrixTracePage, SessionPageWindow) {
    for (int i = 1; i <= 5; ++i) writer_->Write(T("sP", "m", "success", i * 100));
    const TPageRow& p = GetParam();
    agent_trace::TraceFilter f;
    f.limit = p.limit;
    f.offset = p.offset;
    auto res = writer_->Query("sP", f);
    ASSERT_TRUE(res.ok()) << res.status().message();
    EXPECT_EQ(res->total_count, 5);
    EXPECT_EQ(static_cast<int>(res->traces.size()), p.expect);
    EXPECT_EQ(res->has_next, p.has_next);
}
INSTANTIATE_TEST_SUITE_P(
    Windows, OpLogMatrixTracePage,
    ::testing::Values(TPageRow{2, 0, 2, true},
                      TPageRow{2, 2, 2, true},
                      TPageRow{2, 4, 1, false},
                      TPageRow{5, 0, 5, false},
                      TPageRow{10, 0, 5, false}));

// Invalid filter rejection -> CX_ERR_F13_* token.
struct TBadRow { int limit; int offset; const char* token; };
class OpLogMatrixTraceBadFilter
    : public OpLogMatrixTraceFx,
      public ::testing::WithParamInterface<TBadRow> {};
TEST_P(OpLogMatrixTraceBadFilter, RejectedWithToken) {
    writer_->Write(T("sB", "m", "success", 1000));
    const TBadRow& r = GetParam();
    agent_trace::TraceFilter f;
    f.limit = r.limit;
    f.offset = r.offset;
    auto res = writer_->Query("sB", f);
    ASSERT_FALSE(res.ok());
    EXPECT_NE(res.status().message().find(r.token), std::string::npos)
        << res.status().message();
}
INSTANTIATE_TEST_SUITE_P(
    Guards, OpLogMatrixTraceBadFilter,
    ::testing::Values(TBadRow{0, 0, "CX_ERR_F13"},
                      TBadRow{201, 0, "CX_ERR_F13"},
                      TBadRow{50, -1, "CX_ERR_F13"}));

// Query on an unknown session is a valid empty result (not an error).
TEST_F(OpLogMatrixTraceFx, UnknownSessionIsEmptyResult) {
    agent_trace::TraceFilter f;
    auto res = writer_->Query("nope", f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 0);
    EXPECT_TRUE(res->traces.empty());
}

// auto-created_at when entry leaves it 0.
TEST_F(OpLogMatrixTraceFx, AutoCreatedAtWhenZero) {
    agent_trace::AgentTraceEntry e = T("sT", "m", "success", 0);  // 0 -> writer fills now()
    writer_->Write(e);
    agent_trace::TraceFilter f;
    auto res = writer_->Query("sT", f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->traces.size(), 1u);
    EXPECT_GT(res->traces[0].created_at, 0);
}

// TraceContext fallback supplies trace_id only when the entry leaves it unset.
TEST_F(OpLogMatrixTraceFx, TraceContextFallbackForTraceId) {
    observability::TraceContext ctx;
    ctx.trace_id = "ctx-T";
    writer_->Write(T("sC", "m", "success", 1000), &ctx);  // no entry trace_id -> ctx fills
    agent_trace::TraceFilter f;
    auto res = writer_->Query("sC", f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->traces.size(), 1u);
    EXPECT_EQ(res->traces[0].trace_id, std::optional<std::string>("ctx-T"));
}

// Entry's own trace_id wins over the ctx fallback.
TEST_F(OpLogMatrixTraceFx, EntryTraceIdWinsOverContext) {
    observability::TraceContext ctx;
    ctx.trace_id = "ctx-T";
    agent_trace::AgentTraceEntry e = T("sW", "m", "success", 1000);
    e.trace_id = "own-T";
    writer_->Write(e, &ctx);
    agent_trace::TraceFilter f;
    auto res = writer_->Query("sW", f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->traces.size(), 1u);
    EXPECT_EQ(res->traces[0].trace_id, std::optional<std::string>("own-T"));
}

// source matrix: http / mcp both persist and round-trip.
class OpLogMatrixTraceSource
    : public OpLogMatrixTraceFx,
      public ::testing::WithParamInterface<const char*> {};
TEST_P(OpLogMatrixTraceSource, SourceRoundTrips) {
    agent_trace::AgentTraceEntry e = T("sSrc", "m", "success", 1000);
    e.source = GetParam();
    writer_->Write(e);
    agent_trace::TraceFilter f;
    auto res = writer_->Query("sSrc", f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->traces.size(), 1u);
    EXPECT_EQ(res->traces[0].source, GetParam());
}
INSTANTIATE_TEST_SUITE_P(Sources, OpLogMatrixTraceSource,
                         ::testing::Values("http", "mcp"));

// status x source cross-product: every (status, source) cell persists & filters.
struct StatSrcRow { const char* status; const char* source; };
class OpLogMatrixTraceStatusSource
    : public OpLogMatrixTraceFx,
      public ::testing::WithParamInterface<StatSrcRow> {};
TEST_P(OpLogMatrixTraceStatusSource, CellPersistsAndFilters) {
    const StatSrcRow& r = GetParam();
    agent_trace::AgentTraceEntry e = T("sX", "m", r.status, 1000);
    e.source = r.source;
    writer_->Write(e);
    agent_trace::TraceFilter f;
    f.status = r.status;
    auto res = writer_->Query("sX", f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->traces.size(), 1u);
    EXPECT_EQ(res->traces[0].status, r.status);
    EXPECT_EQ(res->traces[0].source, r.source);
}
INSTANTIATE_TEST_SUITE_P(
    Cells, OpLogMatrixTraceStatusSource,
    ::testing::Values(StatSrcRow{"success", "http"},
                      StatSrcRow{"success", "mcp"},
                      StatSrcRow{"failed", "http"},
                      StatSrcRow{"failed", "mcp"},
                      StatSrcRow{"cancelled", "http"},
                      StatSrcRow{"cancelled", "mcp"},
                      StatSrcRow{"session_timeout", "http"},
                      StatSrcRow{"session_timeout", "mcp"}));

// namespace_id filter dimension on traces.
TEST_F(OpLogMatrixTraceFx, FilterByNamespaceId) {
    agent_trace::AgentTraceEntry a = T("sN", "m", "success", 1000);
    a.namespace_id = "sales";
    writer_->Write(a);
    agent_trace::AgentTraceEntry b = T("sN", "m", "success", 2000);
    b.namespace_id = "eng";
    writer_->Write(b);
    agent_trace::TraceFilter f;
    f.namespace_id = "sales";
    auto res = writer_->Query("sN", f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 1);
}

// timestamp range filter on traces.
struct TRangeRow { int64_t from; int64_t to; int expect; };
class OpLogMatrixTraceRange
    : public OpLogMatrixTraceFx,
      public ::testing::WithParamInterface<TRangeRow> {};
TEST_P(OpLogMatrixTraceRange, RangeFiltersCount) {
    for (int i = 1; i <= 5; ++i) writer_->Write(T("sR", "m", "success", i * 1000));
    const TRangeRow& r = GetParam();
    agent_trace::TraceFilter f;
    f.from_timestamp = r.from;
    f.to_timestamp = r.to;
    auto res = writer_->Query("sR", f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, r.expect);
}
INSTANTIATE_TEST_SUITE_P(
    Ranges, OpLogMatrixTraceRange,
    ::testing::Values(TRangeRow{1000, 5000, 5},
                      TRangeRow{2000, 4000, 3},
                      TRangeRow{2500, 3500, 1},
                      TRangeRow{6000, 9000, 0},
                      TRangeRow{1000, 1000, 1},
                      TRangeRow{1500, 4500, 3}));

// OnSessionEnd does not throw and leaves prior rows intact.
TEST_F(OpLogMatrixTraceFx, OnSessionEndIsSafe) {
    writer_->Write(T("sE", "m", "success", 1000));
    writer_->OnSessionEnd("sE");  // no throw; finalize hook
    agent_trace::TraceFilter f;
    auto res = writer_->Query("sE", f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 1);
}

// params / result_summary optional fields round-trip when set.
TEST_F(OpLogMatrixTraceFx, ParamsAndResultSummaryRoundTrip) {
    agent_trace::AgentTraceEntry e = T("sP2", "search", "success", 1000);
    e.params = "{\"q\":\"hi\"}";
    e.result_summary = "5 hits";
    writer_->Write(e);
    agent_trace::TraceFilter f;
    auto res = writer_->Query("sP2", f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->traces.size(), 1u);
    EXPECT_EQ(res->traces[0].params, std::optional<std::string>("{\"q\":\"hi\"}"));
    EXPECT_EQ(res->traces[0].result_summary, std::optional<std::string>("5 hits"));
}

// =====================================================================
// E. ObservabilityContext thread-local + trace lifecycle edges
// =====================================================================

// SetThreadLocal installs identity + trace into the thread-local context.
TEST(OpLogMatrixThreadLocal, SetThreadLocalInstallsTrace) {
    observability::ObservabilityContext::ThreadLocal().ClearTraceContext();
    observability::ObservabilityContext ctx;
    observability::TraceContext tc;
    tc.trace_id = "tl-trace";
    ctx.SetTraceContext(tc);
    ctx.SetThreadLocal();
    const observability::TraceContext* got =
        observability::ObservabilityContext::ThreadLocal().GetTraceContext();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->trace_id, "tl-trace");
    observability::ObservabilityContext::ThreadLocal().ClearTraceContext();
}

// ClearTraceContext drops the trace; GetTraceContext returns nullptr.
TEST(OpLogMatrixThreadLocal, ClearTraceContextResetsToNull) {
    observability::ObservabilityContext ctx;
    observability::TraceContext tc;
    tc.trace_id = "x";
    ctx.SetTraceContext(tc);
    EXPECT_NE(ctx.GetTraceContext(), nullptr);
    ctx.ClearTraceContext();
    EXPECT_EQ(ctx.GetTraceContext(), nullptr);
}

// A fresh context has no trace until one is set.
TEST(OpLogMatrixThreadLocal, FreshContextHasNoTrace) {
    observability::ObservabilityContext ctx;
    EXPECT_EQ(ctx.GetTraceContext(), nullptr);
}

// FormatStructured carries the level + message for every level (matrix).
struct FmtLvlRow { observability::LogLevel lvl; };
class OpLogMatrixFormatLevel : public ::testing::TestWithParam<FmtLvlRow> {};
TEST_P(OpLogMatrixFormatLevel, FormatStructuredEmitsMessage) {
    observability::ObservabilityContext ctx;
    std::string line = ctx.FormatStructured(GetParam().lvl, "payload-msg");
    EXPECT_NE(line.find("payload-msg"), std::string::npos);
    EXPECT_NE(line.find(observability::ToString(GetParam().lvl)), std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(
    Levels, OpLogMatrixFormatLevel,
    ::testing::Values(FmtLvlRow{observability::LogLevel::kDebug},
                      FmtLvlRow{observability::LogLevel::kInfo},
                      FmtLvlRow{observability::LogLevel::kWarn},
                      FmtLvlRow{observability::LogLevel::kError}));

// =====================================================================
// F. OperationLog optional-field round-trip + summary/session edges
// =====================================================================

// summary + session_id optional fields persist and round-trip.
TEST_F(OpLogMatrixLoggerFx, SummaryAndSessionIdRoundTrip) {
    observability::OperationLogEntry e = E("alice", "memory_create", "memory", 1000);
    e.summary = "[auto] User likes coffee";
    e.session_id = "sess-77";
    e.resource_id = "block-9";
    logger_->Log(e);
    observability::OperationLogFilter f;
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->entries.size(), 1u);
    EXPECT_EQ(res->entries[0].summary, std::optional<std::string>("[auto] User likes coffee"));
    EXPECT_EQ(res->entries[0].session_id, std::optional<std::string>("sess-77"));
    EXPECT_EQ(res->entries[0].resource_id, std::optional<std::string>("block-9"));
}

// Unset optionals come back as nullopt (NULL columns).
TEST_F(OpLogMatrixLoggerFx, UnsetOptionalsAreNullopt) {
    logger_->Log(E("u", "query_run", "query", 1000));  // no ns/summary/trace/session
    observability::OperationLogFilter f;
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->entries.size(), 1u);
    EXPECT_FALSE(res->entries[0].namespace_id.has_value());
    EXPECT_FALSE(res->entries[0].summary.has_value());
    EXPECT_FALSE(res->entries[0].trace_id.has_value());
    EXPECT_FALSE(res->entries[0].session_id.has_value());
}

// Auto-timestamp fills when entry leaves it 0; id is DB-assigned > 0.
TEST_F(OpLogMatrixLoggerFx, AutoTimestampAndId) {
    logger_->Log(E("u", "query_run", "query", 0));  // 0 -> now()
    observability::OperationLogFilter f;
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res->entries.size(), 1u);
    EXPECT_GT(res->entries[0].timestamp, 0);
    EXPECT_GT(res->entries[0].id, 0);
}

// resource_type filter dimension matrix.
class OpLogMatrixResourceFilter
    : public OpLogMatrixLoggerFx,
      public ::testing::WithParamInterface<const char*> {};
TEST_P(OpLogMatrixResourceFilter, FilterIsolatesResourceType) {
    logger_->Log(E("u", "document_upload", "document", 1000));
    logger_->Log(E("u", "namespace_create", "namespace", 2000));
    logger_->Log(E("u", "memory_create", "memory", 3000));
    observability::OperationLogFilter f;
    f.resource_type = GetParam();
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res->total_count, 1);
    if (!res->entries.empty()) EXPECT_EQ(res->entries[0].resource_type, GetParam());
}
INSTANTIATE_TEST_SUITE_P(Each, OpLogMatrixResourceFilter,
                         ::testing::Values("document", "namespace", "memory"));

// next_offset projection matrix: offset + returned-rows for partial/full pages.
struct NextOffRow { int limit; int offset; int next_offset; };
class OpLogMatrixNextOffset
    : public OpLogMatrixLoggerFx,
      public ::testing::WithParamInterface<NextOffRow> {};
TEST_P(OpLogMatrixNextOffset, NextOffsetProjection) {
    for (int i = 1; i <= 5; ++i) logger_->Log(E("u", "query_run", "query", i * 100));
    const NextOffRow& r = GetParam();
    observability::OperationLogFilter f;
    f.limit = r.limit;
    f.offset = r.offset;
    auto res = logger_->Query(f);
    ASSERT_TRUE(res.ok()) << res.status().message();
    EXPECT_EQ(res->next_offset, r.next_offset);
}
INSTANTIATE_TEST_SUITE_P(
    Windows, OpLogMatrixNextOffset,
    ::testing::Values(NextOffRow{2, 0, 2},
                      NextOffRow{2, 2, 4},
                      NextOffRow{2, 4, 5},
                      NextOffRow{5, 0, 5}));

}  // namespace
}  // namespace cortrix
