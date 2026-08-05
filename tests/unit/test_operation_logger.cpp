#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/catalog/schema_provider.h"  // SchemaMigrator
#include "cortrix/observability/operation_log_error.h"
#include "cortrix/observability/operation_log_schema.h"
#include "cortrix/observability/operation_logger_impl.h"
#include "cortrix/observability/oplog_metrics.h"  // §11 emit-integration assertions

// S2 coverage: OperationLogger CE implementation — Log / BatchLog
// (all-or-nothing) / Query (filters + pagination + validation) / Cleanup
// (retention + row-cap) / GetStats / Health.
namespace cortrix::observability {
namespace {

// Fixture: in-memory global db migrated with the operation log provider, an
// InMemoryGlobalConfig, and an OperationLogger over the handle.
class OperationLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        config_ = std::make_shared<InMemoryGlobalConfig>();
        logger_ = std::make_unique<OperationLogger>(db_, config_);
    }
    void TearDown() override {
        logger_.reset();
        if (db_) sqlite3_close(db_);
    }

    // Make an entry with the common required fields filled.
    OperationLogEntry Entry(const std::string& user, const std::string& action,
                            const std::string& rtype, int64_t ts = 0) {
        OperationLogEntry e;
        e.timestamp = ts;
        e.user_id = user;
        e.action = action;
        e.resource_type = rtype;
        return e;
    }

    int64_t RowCount() {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM operation_log", -1, &s, nullptr);
        int64_t n = 0;
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        return n;
    }

    OperationLogSchemaProvider provider_;
    sqlite3* db_ = nullptr;
    std::shared_ptr<InMemoryGlobalConfig> config_;
    std::unique_ptr<OperationLogger> logger_;
};

// Log writes one row; auto-timestamp fills when 0; optional fields persist.
TEST_F(OperationLoggerTest, LogWritesRowAndAutoTimestamp) {
    OperationLogEntry e = Entry("alice", "memory_create", "memory");
    e.namespace_id = "sales";
    e.resource_id = "block-1";
    e.summary = "User likes coffee";
    e.trace_id = "trace-001";
    logger_->Log(e);

    ASSERT_EQ(RowCount(), 1);
    OperationLogFilter f;
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok()) << r.status().message();
    ASSERT_EQ(r->entries.size(), 1u);
    const auto& got = r->entries[0];
    EXPECT_EQ(got.user_id, "alice");
    EXPECT_EQ(got.action, "memory_create");
    EXPECT_EQ(got.namespace_id, std::optional<std::string>("sales"));
    EXPECT_EQ(got.trace_id, std::optional<std::string>("trace-001"));
    EXPECT_FALSE(got.session_id.has_value());  // not set → NULL → nullopt
    EXPECT_GT(got.timestamp, 0);               // auto-filled
    EXPECT_GT(got.id, 0);
}

// Log uses the TraceContext fallback only when the entry leaves trace_id unset.
TEST_F(OperationLoggerTest, LogTraceContextFallback) {
    TraceContext ctx;
    ctx.trace_id = "ctx-trace";
    // entry has no trace_id → ctx fills it
    logger_->Log(Entry("u", "query", "query"), &ctx);
    // entry has its own trace_id → ctx does NOT override
    OperationLogEntry e2 = Entry("u", "query", "query");
    e2.trace_id = "own-trace";
    logger_->Log(e2, &ctx);

    OperationLogFilter f;
    f.sort_order = "ASC";
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->entries.size(), 2u);
    EXPECT_EQ(r->entries[0].trace_id, std::optional<std::string>("ctx-trace"));
    EXPECT_EQ(r->entries[1].trace_id, std::optional<std::string>("own-trace"));
}

// BatchLog commits all rows in one transaction.
TEST_F(OperationLoggerTest, BatchLogAllOrNothingCommit) {
    std::vector<OperationLogEntry> batch = {
        Entry("a", "upload", "document"),
        Entry("a", "delete", "document"),
        Entry("b", "ns_create", "namespace"),
    };
    logger_->BatchLog(batch);
    EXPECT_EQ(RowCount(), 3);
}

TEST_F(OperationLoggerTest, EmptyBatchIsNoOp) {
    logger_->BatchLog({});
    EXPECT_EQ(RowCount(), 0);
}

// BatchLog is all-or-nothing: if any insert fails mid-batch the whole transaction
// rolls back (Issue 4). We force a deterministic failure by adding a temporary
// UNIQUE index on timestamp, then batching two entries with the same timestamp —
// the second INSERT violates the constraint, so neither row should persist.
TEST_F(OperationLoggerTest, BatchLogRollsBackOnMidBatchFailure) {
    ASSERT_EQ(sqlite3_exec(db_,
        "CREATE UNIQUE INDEX ux_test_ts ON operation_log(timestamp)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);

    std::vector<OperationLogEntry> batch = {
        Entry("a", "upload", "document", 7000),
        Entry("a", "delete", "document", 7000),  // duplicate ts → UNIQUE violation
        Entry("a", "query",  "query",    8000),
    };
    logger_->BatchLog(batch);
    EXPECT_EQ(RowCount(), 0) << "partial batch must roll back entirely";

    // Health surfaces the last write error after the failed batch.
    EXPECT_FALSE(logger_->Health().last_error.empty());

    sqlite3_exec(db_, "DROP INDEX ux_test_ts", nullptr, nullptr, nullptr);
}

// ---- Query: filters ----

TEST_F(OperationLoggerTest, QueryFilterByActionAndUser) {
    logger_->Log(Entry("alice", "memory_create", "memory"));
    logger_->Log(Entry("alice", "memory_delete", "memory"));
    logger_->Log(Entry("bob", "memory_create", "memory"));

    OperationLogFilter f;
    f.action = "memory_create";
    f.user_id = "alice";
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->total_count, 1);
    ASSERT_EQ(r->entries.size(), 1u);
    EXPECT_EQ(r->entries[0].user_id, "alice");
    EXPECT_EQ(r->entries[0].action, "memory_create");
}

TEST_F(OperationLoggerTest, QueryActionInMultiSelect) {
    logger_->Log(Entry("u", "memory_create", "memory"));
    logger_->Log(Entry("u", "memory_edit", "memory"));
    logger_->Log(Entry("u", "memory_delete", "memory"));
    logger_->Log(Entry("u", "query", "query"));

    OperationLogFilter f;
    f.action_in = std::vector<std::string>{"memory_create", "memory_delete"};
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->total_count, 2);
}

TEST_F(OperationLoggerTest, QueryTimestampRangeAndTraceId) {
    logger_->Log(Entry("u", "query", "query", 1000));
    logger_->Log(Entry("u", "query", "query", 2000));
    OperationLogEntry e = Entry("u", "query", "query", 3000);
    e.trace_id = "T3";
    logger_->Log(e);

    OperationLogFilter f;
    f.from_timestamp = 1500;
    f.to_timestamp = 3500;
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->total_count, 2);

    OperationLogFilter ft;
    ft.trace_id = "T3";
    auto rt = logger_->Query(ft);
    ASSERT_TRUE(rt.ok());
    ASSERT_EQ(rt->entries.size(), 1u);
    EXPECT_EQ(rt->entries[0].timestamp, 3000);
}

// ---- Query: pagination + sort ----

TEST_F(OperationLoggerTest, QueryPaginationAndSortOrder) {
    for (int i = 1; i <= 5; ++i)
        logger_->Log(Entry("u", "query", "query", i * 100));

    OperationLogFilter f;
    f.limit = 2;
    f.offset = 0;
    f.sort_order = "DESC";
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->total_count, 5);
    ASSERT_EQ(r->entries.size(), 2u);
    EXPECT_EQ(r->entries[0].timestamp, 500);  // newest first
    EXPECT_EQ(r->entries[1].timestamp, 400);
    EXPECT_TRUE(r->has_next);
    EXPECT_EQ(r->next_offset, 2);

    // second page
    f.offset = 2;
    auto r2 = logger_->Query(f);
    ASSERT_TRUE(r2.ok());
    ASSERT_EQ(r2->entries.size(), 2u);
    EXPECT_EQ(r2->entries[0].timestamp, 300);
    EXPECT_TRUE(r2->has_next);

    // last (partial) page
    f.offset = 4;
    auto r3 = logger_->Query(f);
    ASSERT_TRUE(r3.ok());
    ASSERT_EQ(r3->entries.size(), 1u);
    EXPECT_EQ(r3->entries[0].timestamp, 100);
    EXPECT_FALSE(r3->has_next);
    EXPECT_EQ(r3->next_offset, 5);
}

// ---- Query: validation → CX_ERR_OPLOG_* ----

TEST_F(OperationLoggerTest, QueryInvalidLimitRejected) {
    OperationLogFilter f;
    f.limit = 500;  // > 200 cap
    auto r = logger_->Query(f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_OPLOG_INVALID_FILTER"), std::string::npos);
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(OperationLoggerTest, QueryBadSortOrderRejected) {
    OperationLogFilter f;
    f.sort_order = "SIDEWAYS";
    auto r = logger_->Query(f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_OPLOG_INVALID_FILTER"), std::string::npos);
}

TEST_F(OperationLoggerTest, QueryInvertedTimestampRangeRejected) {
    OperationLogFilter f;
    f.from_timestamp = 5000;
    f.to_timestamp = 1000;
    auto r = logger_->Query(f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE"),
              std::string::npos);
}

TEST_F(OperationLoggerTest, QueryEmptyActionInRejected) {
    OperationLogFilter f;
    f.action_in = std::vector<std::string>{};
    auto r = logger_->Query(f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_OPLOG_INVALID_FILTER"), std::string::npos);
}

TEST_F(OperationLoggerTest, QueryOffsetPastEndIsOutOfRange) {
    logger_->Log(Entry("u", "query", "query"));
    logger_->Log(Entry("u", "query", "query"));
    OperationLogFilter f;
    f.offset = 50;  // >= total_count (2)
    auto r = logger_->Query(f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE"),
              std::string::npos);
}

TEST_F(OperationLoggerTest, QueryEmptySetOffsetZeroIsValidEmptyPage) {
    OperationLogFilter f;  // offset 0 on empty table → valid empty page, not error
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->total_count, 0);
    EXPECT_TRUE(r->entries.empty());
    EXPECT_FALSE(r->has_next);
}

// ---- Cleanup: retention + row-cap ----

TEST_F(OperationLoggerTest, CleanupDeletesByRetentionAge) {
    config_->operation_log_retention_days = 30;
    // now ~ system time; insert one row well past retention and one fresh.
    int64_t now = 0;
    {  // read back a fresh row's timestamp to anchor "now"
        logger_->Log(Entry("u", "query", "query"));  // auto-now
        auto r = logger_->Query(OperationLogFilter{});
        now = r->entries[0].timestamp;
    }
    int64_t old_ts = now - 40LL * 86400000LL;  // 40 days ago > 30d retention
    logger_->Log(Entry("u", "query", "query", old_ts));
    EXPECT_EQ(RowCount(), 2);

    logger_->Cleanup();
    EXPECT_EQ(RowCount(), 1);                 // old row purged
    EXPECT_EQ(logger_->last_cleanup_deleted(), 1);
}

TEST_F(OperationLoggerTest, CleanupEnforcesRowCapKeepingNewest) {
    config_->operation_log_retention_days = 36500;  // effectively no age purge
    config_->operation_log_max_rows = 3;
    for (int i = 1; i <= 6; ++i)
        logger_->Log(Entry("u", "query", "query", i * 1000));  // ts 1000..6000
    EXPECT_EQ(RowCount(), 6);

    logger_->Cleanup();
    EXPECT_EQ(RowCount(), 3);  // oldest 3 deleted, newest 3 kept

    OperationLogFilter f;
    f.sort_order = "ASC";
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->entries.size(), 3u);
    EXPECT_EQ(r->entries[0].timestamp, 4000);  // 1000/2000/3000 purged
    EXPECT_EQ(r->entries[2].timestamp, 6000);
}

TEST_F(OperationLoggerTest, CleanupRowCapZeroIsUnbounded) {
    config_->operation_log_retention_days = 36500;
    config_->operation_log_max_rows = 0;  // Ent: unbounded
    for (int i = 1; i <= 5; ++i) logger_->Log(Entry("u", "query", "query", i * 1000));
    logger_->Cleanup();
    EXPECT_EQ(RowCount(), 5);  // nothing purged
}

// ---- Query: remaining filter predicates + offset validation ----

// namespace_id and resource_type predicates (the only add_text filters not yet
// individually exercised) each scope the result set.
TEST_F(OperationLoggerTest, QueryFilterByNamespaceAndResourceType) {
    OperationLogEntry a = Entry("u", "query", "query");
    a.namespace_id = "sales";
    logger_->Log(a);
    OperationLogEntry b = Entry("u", "upload", "document");
    b.namespace_id = "eng";
    logger_->Log(b);

    OperationLogFilter byns;
    byns.namespace_id = "sales";
    auto rns = logger_->Query(byns);
    ASSERT_TRUE(rns.ok());
    EXPECT_EQ(rns->total_count, 1);
    EXPECT_EQ(rns->entries[0].namespace_id, std::optional<std::string>("sales"));

    OperationLogFilter brt;
    brt.resource_type = "document";
    auto rrt = logger_->Query(brt);
    ASSERT_TRUE(rrt.ok());
    EXPECT_EQ(rrt->total_count, 1);
    EXPECT_EQ(rrt->entries[0].resource_type, "document");
}

// A negative offset is an input fault (the offset < 0 validation branch).
TEST_F(OperationLoggerTest, QueryNegativeOffsetRejected) {
    OperationLogFilter f;
    f.offset = -1;
    auto r = logger_->Query(f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_OPLOG_INVALID_FILTER"), std::string::npos);
}

// A limit below the minimum (e.g. 0) is rejected by the same [1,200] guard (the
// limit < 1 half of the OR, distinct from the limit > 200 case already covered).
TEST_F(OperationLoggerTest, QueryZeroLimitRejected) {
    OperationLogFilter f;
    f.limit = 0;
    auto r = logger_->Query(f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_OPLOG_INVALID_FILTER"), std::string::npos);
}

// A from_timestamp without to_timestamp does not trip the inverted-range check
// (the to_timestamp.has_value() half of the AND is false) and filters normally.
TEST_F(OperationLoggerTest, QueryFromTimestampOnlyNoRangeError) {
    logger_->Log(Entry("u", "query", "query", 1000));
    logger_->Log(Entry("u", "query", "query", 3000));
    OperationLogFilter f;
    f.from_timestamp = 2000;  // no to_timestamp
    auto r = logger_->Query(f);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->total_count, 1);
    EXPECT_EQ(r->entries[0].timestamp, 3000);
}

// ---- Cleanup: null-config default branch ----

// An OperationLogger built with a null config uses the built-in defaults
// (retention_days=30, max_rows=100000) in Cleanup (the config_ ? ... : default
// ternaries). With only fresh rows, nothing is purged.
TEST_F(OperationLoggerTest, CleanupWithNullConfigUsesDefaults) {
    OperationLogger no_cfg(db_, /*config=*/nullptr);
    no_cfg.Log(Entry("u", "query", "query"));  // fresh row, within default 30d
    no_cfg.Cleanup();
    EXPECT_EQ(RowCount(), 1);  // default retention keeps it, default cap not hit
    EXPECT_EQ(no_cfg.last_cleanup_deleted(), 0);
}

// ---- GetStats / Health ----

TEST_F(OperationLoggerTest, GetStatsReportsCountAndRange) {
    logger_->Log(Entry("u", "query", "query", 1000));
    logger_->Log(Entry("u", "query", "query", 5000));
    auto s = logger_->GetStats();
    EXPECT_EQ(s.total_count, 2);
    EXPECT_EQ(s.oldest_timestamp, 1000);
    EXPECT_EQ(s.latest_timestamp, 5000);
    EXPECT_GE(s.size_bytes, 0);
}

TEST_F(OperationLoggerTest, HealthReportsReachableWhenOk) {
    auto h = logger_->Health();
    EXPECT_TRUE(h.db_path_reachable);
    EXPECT_TRUE(h.is_healthy);
    EXPECT_TRUE(h.last_error.empty());
    EXPECT_EQ(h.last_cleanup_timestamp, 0);  // no cleanup yet

    logger_->Cleanup();
    auto h2 = logger_->Health();
    EXPECT_GT(h2.last_cleanup_timestamp, 0);
}

// A logger over a null db handle reports unreachable / unhealthy (the
// db_ != nullptr guard in Health() is false) without crashing.
TEST(OperationLoggerNullDbTest, HealthUnreachableWithNullHandle) {
    auto cfg = std::make_shared<InMemoryGlobalConfig>();
    OperationLogger logger(/*db=*/nullptr, cfg);
    auto h = logger.Health();
    EXPECT_FALSE(h.db_path_reachable);
    EXPECT_FALSE(h.is_healthy);
}

// ---- prepare-failure arms: drop the table so every SELECT/DELETE prepare fails -
//
// The validation arms are covered above; these reach the `prepare_v2 != SQLITE_OK`
// arms inside Query/Cleanup/GetStats (CX_ERR_OPLOG_INTERNAL / silent skip). With
// `tasks`... here `operation_log` dropped from the shared handle, every prepared
// statement referencing it fails to prepare.
TEST_F(OperationLoggerTest, QueryCountPrepareFailureIsInternal) {
    logger_->Log(Entry("u", "query", "query"));  // table still present
    ASSERT_EQ(sqlite3_exec(db_, "DROP TABLE operation_log", nullptr, nullptr, nullptr),
              SQLITE_OK);
    OperationLogFilter f;  // valid filter → reaches the count query, which can't prepare
    auto r = logger_->Query(f);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_OPLOG_INTERNAL"), std::string::npos);
}

// GetStats swallows prepare failures (each guarded by `== SQLITE_OK`), returning a
// zero-filled stats struct rather than throwing.
TEST_F(OperationLoggerTest, GetStatsToleratesMissingTable) {
    ASSERT_EQ(sqlite3_exec(db_, "DROP TABLE operation_log", nullptr, nullptr, nullptr),
              SQLITE_OK);
    auto s = logger_->GetStats();  // count prepare fails → defaults retained
    EXPECT_EQ(s.total_count, 0);
    EXPECT_EQ(s.oldest_timestamp, 0);
}

// Cleanup's DELETE prepare fails closed (the `== SQLITE_OK` guard is false) and
// is SILENTLY tolerated: the whole DELETE block is skipped, so no rows delete and
// no last_error_ is recorded (only a step failure records one).
TEST_F(OperationLoggerTest, CleanupToleratesMissingTable) {
    config_->operation_log_max_rows = 0;  // skip the row-cap branch (no count query)
    ASSERT_EQ(sqlite3_exec(db_, "DROP TABLE operation_log", nullptr, nullptr, nullptr),
              SQLITE_OK);
    logger_->Cleanup();
    EXPECT_EQ(logger_->last_cleanup_deleted(), 0);
    EXPECT_TRUE(logger_->Health().last_error.empty());  // prepare failure is silent
}

// Cleanup's age-DELETE step failure (prepares fine, fails at step) via a BEFORE
// DELETE trigger that aborts. The row-cap branch is disabled so only the age DELETE
// runs. Exercises the `step == SQLITE_DONE` else arm (last_error_ set, deleted 0).
TEST_F(OperationLoggerTest, CleanupAgeDeleteStepFailureRecordsError) {
    config_->operation_log_retention_days = 1;
    config_->operation_log_max_rows = 0;
    // a row old enough to be in the DELETE's range
    logger_->Log(Entry("u", "query", "query", 1000));
    ASSERT_EQ(sqlite3_exec(db_,
        "CREATE TRIGGER t_block_oplog_del BEFORE DELETE ON operation_log "
        "BEGIN SELECT RAISE(ABORT,'blocked'); END;", nullptr, nullptr, nullptr),
        SQLITE_OK);
    logger_->Cleanup();
    EXPECT_EQ(logger_->last_cleanup_deleted(), 0);
    EXPECT_FALSE(logger_->Health().last_error.empty());
    sqlite3_exec(db_, "DROP TRIGGER t_block_oplog_del", nullptr, nullptr, nullptr);
}

// Cleanup's row-cap DELETE step failure: keep age retention effectively infinite so
// only the row-cap path runs, seed > max_rows rows, then abort the DELETE at step.
TEST_F(OperationLoggerTest, CleanupRowCapDeleteStepFailureRecordsError) {
    config_->operation_log_retention_days = 36500;  // no age purge
    config_->operation_log_max_rows = 2;
    for (int i = 1; i <= 5; ++i) logger_->Log(Entry("u", "query", "query", i * 1000));
    ASSERT_EQ(sqlite3_exec(db_,
        "CREATE TRIGGER t_block_oplog_cap BEFORE DELETE ON operation_log "
        "BEGIN SELECT RAISE(ABORT,'blocked'); END;", nullptr, nullptr, nullptr),
        SQLITE_OK);
    logger_->Cleanup();
    EXPECT_FALSE(logger_->Health().last_error.empty());
    sqlite3_exec(db_, "DROP TRIGGER t_block_oplog_cap", nullptr, nullptr, nullptr);
}

// BatchLog's BEGIN failure arm: open a transaction on the shared handle first, so
// the logger's own "BEGIN" fails (nested transactions are not allowed), and the
// batch returns early without inserting.
TEST_F(OperationLoggerTest, BatchLogBeginFailureAborts) {
    ASSERT_EQ(sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
    logger_->BatchLog({Entry("a", "upload", "document")});
    // The outer txn is still open and nothing was committed by the logger.
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    EXPECT_EQ(RowCount(), 0);
    EXPECT_FALSE(logger_->Health().last_error.empty());
}

// ---- §11 OBS_SPEC metric emit-integration (real Log/Query/Cleanup drive the
// process-wide OplogMetrics recorder) -------------------------------------------

// A successful Log() bumps cortrix_oplog_writes_total{action, resource_type}.
TEST_F(OperationLoggerTest, LogEmitsWritesMetric) {
    OplogMetrics::Instance().ResetForTest();
    logger_->Log(Entry("u1", "query", "query"));
    logger_->Log(Entry("u1", "query", "query"));
    logger_->Log(Entry("u1", "namespace_create", "namespace"));
    EXPECT_EQ(OplogMetrics::Instance().WriteCount("query", "query"), 2u);
    EXPECT_EQ(OplogMetrics::Instance().WriteCount("namespace_create", "namespace"), 1u);
    OplogMetrics::Instance().ResetForTest();
}

// Query() observes cortrix_oplog_query_latency_seconds at the active-filter count.
TEST_F(OperationLoggerTest, QueryEmitsLatencyMetricWithFilterDimensions) {
    OplogMetrics::Instance().ResetForTest();
    logger_->Log(Entry("u1", "query", "query"));

    OperationLogFilter f0;  // no filters → filter_dimensions = 0
    ASSERT_TRUE(logger_->Query(f0).ok());
    EXPECT_EQ(OplogMetrics::Instance().QueryLatencyCount(0), 1u);

    OperationLogFilter f2;  // action + resource_type → filter_dimensions = 2
    f2.action = "query";
    f2.resource_type = "query";
    ASSERT_TRUE(logger_->Query(f2).ok());
    EXPECT_EQ(OplogMetrics::Instance().QueryLatencyCount(2), 1u);

    // A validation-rejected query is NOT a latency sample.
    OperationLogFilter bad;
    bad.limit = 9999;  // > 200 → kInvalidFilter before any DB work
    EXPECT_FALSE(logger_->Query(bad).ok());
    EXPECT_EQ(OplogMetrics::Instance().QueryLatencyCount(0), 1u);  // unchanged
    OplogMetrics::Instance().ResetForTest();
}

// Cleanup() emits cleanup_deleted_total{reason=age} for the retention sweep, plus
// the duration histogram and the size_rows gauge.
TEST_F(OperationLoggerTest, CleanupEmitsAgeDeletedAndSizeMetrics) {
    OplogMetrics::Instance().ResetForTest();
    config_->operation_log_retention_days = 1;  // 1-day window
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    logger_->Log(Entry("u1", "query", "query", 1000));  // ~1970 → age-deleted
    logger_->Log(Entry("u1", "query", "query", now));   // fresh → kept

    logger_->Cleanup();

    EXPECT_EQ(OplogMetrics::Instance().CleanupDeletedCount(OplogMetrics::CleanupReason::kAge), 1u);
    EXPECT_GE(OplogMetrics::Instance().CleanupDurationCount(), 1u);
    EXPECT_EQ(OplogMetrics::Instance().SizeRows(), 1);  // one fresh row remains
    OplogMetrics::Instance().ResetForTest();
}

}  // namespace
}  // namespace cortrix::observability
