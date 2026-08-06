#include <gtest/gtest.h>

#include <sqlite3.h>

#include <ctime>
#include <string>

#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/interaction_sources_schema.h"
#include "cortrix/agent_trace/interactions_handler.h"
#include "cortrix/catalog/schema_provider.h"

// S5 coverage: GET /interactions/{id}/sources business logic — permission
// (own vs admin vs UNAUTHORIZED), INTERACTION_NOT_FOUND, deleted-source counting
// (source_block_id gone from blocks), snippet truncation, and the
// interaction_sources CE schema (no highlight, FK to interaction_log.id TEXT).
namespace cortrix::agent_trace {
namespace {

void Exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "") << " :: " << sql;
    sqlite3_free(err);
}

class InteractionsHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        AgentTraceMetrics::Instance().ResetForTest();
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);

        // The real MVP interaction_log (role/content model, id TEXT PK).
        Exec(db_, R"(
            CREATE TABLE interaction_log (
                id TEXT PRIMARY KEY, session_id TEXT NOT NULL, namespace_name TEXT NOT NULL,
                user_id TEXT, role TEXT NOT NULL, content TEXT NOT NULL, query_type TEXT,
                status TEXT, latency_ms INTEGER DEFAULT 0, metadata_json TEXT,
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
            );)");
        // A minimal blocks table for the deleted-source probe.
        Exec(db_, "CREATE TABLE blocks (block_id TEXT PRIMARY KEY);");
        // interaction_sources via the real agent trace provider.
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());

        handler_ = std::make_unique<InteractionsHandler>(db_);

        // Seed: interaction int-1 owned by alice; 3 source rows; block b1/b2 exist,
        // b3 deleted (not in blocks).
        Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content) "
                  "VALUES('int-1','sess-1','sales','alice','user','q');");
        Exec(db_, "INSERT INTO blocks(block_id) VALUES('b1'),('b2');");
        Exec(db_, "INSERT INTO interaction_sources(interaction_id, source_block_id, source_type, relevance_score, snippet) "
                  "VALUES('int-1','b1','block',0.9,'top snippet'),"
                  "      ('int-1','b2','memory',0.5,'mid snippet'),"
                  "      ('int-1','b3','block',0.7,'gone snippet');");
    }
    void TearDown() override {
        handler_.reset();
        if (db_) sqlite3_close(db_);
        AgentTraceMetrics::Instance().ResetForTest();
    }

    InteractionSourcesSchemaProvider provider_;
    std::unique_ptr<InteractionsHandler> handler_;
    sqlite3* db_ = nullptr;
};

TEST_F(InteractionsHandlerTest, SchemaProviderIdentity) {
    InteractionSourcesSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "interaction_sources");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST_F(InteractionsHandlerTest, OwnerGetsLiveSourcesAndDeletedCount) {
    RequesterContext ctx{"alice", false};
    auto r = handler_->GetSources("int-1", ctx);
    ASSERT_TRUE(r.ok()) << r.status().message();
    const auto& v = r.value();
    EXPECT_EQ(v.interaction_id, "int-1");
    // b1 + b2 live, b3 deleted.
    EXPECT_EQ(v.source_count, 2);
    EXPECT_EQ(v.deleted_sources_count, 1);
    ASSERT_EQ(v.sources.size(), 2u);
    // ordered by relevance DESC: b1 (0.9) then b2 (0.5).
    EXPECT_EQ(v.sources[0].source_block_id, "b1");
    EXPECT_EQ(v.sources[0].source_type, "block");
    EXPECT_DOUBLE_EQ(v.sources[0].relevance_score, 0.9);
    EXPECT_EQ(v.sources[1].source_block_id, "b2");
    // metric counted under the interactions_sources endpoint, user role.
    EXPECT_EQ(AgentTraceMetrics::Instance().TracesQueryCount(
                  AgentTraceMetrics::Role::kUser,
                  AgentTraceMetrics::Endpoint::kInteractionsSources), 1u);
}

TEST_F(InteractionsHandlerTest, AdminCanReadOtherUsersInteraction) {
    RequesterContext admin{"bob", true};
    testing::internal::CaptureStderr();
    auto r = handler_->GetSources("int-1", admin);  // owned by alice
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().source_count, 2);
    EXPECT_EQ(AgentTraceMetrics::Instance().TracesQueryCount(
                  AgentTraceMetrics::Role::kAdmin,
                  AgentTraceMetrics::Endpoint::kInteractionsSources), 1u);
    // forensics line for admin bob reading alice's interaction.
    EXPECT_NE(err.find("admin_cross_user_access"), std::string::npos);
    EXPECT_NE(err.find("\"target_user_id\":\"alice\""), std::string::npos);
}

TEST_F(InteractionsHandlerTest, NonAdminCrossUserIsUnauthorized) {
    RequesterContext mallory{"mallory", false};
    auto r = handler_->GetSources("int-1", mallory);  // owned by alice
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kPermissionDenied);
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_UNAUTHORIZED"), std::string::npos);
}

TEST_F(InteractionsHandlerTest, MissingInteractionIsNotFound) {
    RequesterContext ctx{"alice", false};
    auto r = handler_->GetSources("does-not-exist", ctx);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INTERACTION_NOT_FOUND"), std::string::npos);
}

TEST_F(InteractionsHandlerTest, AllSourcesDeletedYieldsEmptyListWithCount) {
    Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content) "
              "VALUES('int-2','s','sales','alice','user','q');");
    Exec(db_, "INSERT INTO interaction_sources(interaction_id, source_block_id, source_type, relevance_score, snippet) "
              "VALUES('int-2','ghost','block',0.5,'x');");
    auto r = handler_->GetSources("int-2", RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().source_count, 0);
    EXPECT_EQ(r.value().deleted_sources_count, 1);
}

TEST_F(InteractionsHandlerTest, TruncateSnippetKeepsHeadAndTail) {
    EXPECT_EQ(InteractionsHandler::TruncateSnippet("short"), "short");
    std::string big(900, 'a');
    std::string out = InteractionsHandler::TruncateSnippet(big);
    EXPECT_LT(out.size(), big.size());
    EXPECT_NE(out.find("[...]"), std::string::npos);
    EXPECT_EQ(out.substr(0, 5), std::string(5, 'a'));
    EXPECT_LE(out.size(), 400u + 5u + 100u);
}

// A NULL interaction_log.user_id is treated as owned-by-nobody: a non-admin
// requester with empty user_id matches it; a named non-admin does not.
TEST_F(InteractionsHandlerTest, NullOwnerAnonymousScope) {
    Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content) "
              "VALUES('int-anon','s','sales',NULL,'user','q');");
    // named non-admin cannot read an unowned interaction.
    auto denied = handler_->GetSources("int-anon", RequesterContext{"alice", false});
    EXPECT_FALSE(denied.ok());
    // admin can.
    auto ok = handler_->GetSources("int-anon", RequesterContext{"root", true});
    EXPECT_TRUE(ok.ok());
}

// ===== S11: GET /interactions list =====

// Seed a handful of interactions across users/sessions/namespaces/times for the
// list tests. (int-1 from SetUp is alice/sales already.)
class InteractionsListTest : public InteractionsHandlerTest {
protected:
    void SetUp() override {
        InteractionsHandlerTest::SetUp();
        // alice has int-1 (sales, from base) + two more; bob has one.
        Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content, query_type, created_at) "
                  "VALUES('a2','sess-1','sales','alice','user','q2','semantic','2026-05-02T00:00:00.000Z');");
        Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content, query_type, created_at) "
                  "VALUES('a3','sess-2','support','alice','user','q3','chat','2026-05-03T00:00:00.000Z');");
        Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content, query_type, created_at) "
                  "VALUES('b1','sess-9','sales','bob','user','bq','sql','2026-05-04T00:00:00.000Z');");
        // pin int-1's created_at so ordering is deterministic.
        Exec(db_, "UPDATE interaction_log SET created_at='2026-05-01T00:00:00.000Z' WHERE id='int-1';");
    }
};

TEST_F(InteractionsListTest, NonAdminSeesOnlyOwnRows) {
    InteractionListFilter f;
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().total_count, 3);  // int-1, a2, a3 — not bob's
    for (const auto& it : r.value().interactions) EXPECT_EQ(it.user_id, "alice");
    // DESC by created_at: a3 (05-03) first, int-1 (05-01) last.
    ASSERT_EQ(r.value().interactions.size(), 3u);
    EXPECT_EQ(r.value().interactions.front().interaction_id, "a3");
    EXPECT_EQ(r.value().interactions.back().interaction_id, "int-1");
    // query_text is the real `content`, returned in full.
    EXPECT_EQ(r.value().interactions.front().query_text, "q3");
    EXPECT_EQ(r.value().interactions.front().namespace_id, "support");  // == namespace_name
    EXPECT_EQ(AgentTraceMetrics::Instance().TracesQueryCount(
                  AgentTraceMetrics::Role::kUser, AgentTraceMetrics::Endpoint::kInteractions), 1u);
}

TEST_F(InteractionsListTest, NonAdminUserIdFilterForOtherIsUnauthorized) {
    InteractionListFilter f;
    f.user_id = "bob";  // alice asking for bob's
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_UNAUTHORIZED"), std::string::npos);
}

TEST_F(InteractionsListTest, NonAdminUserIdFilterForSelfIsAllowed) {
    InteractionListFilter f;
    f.user_id = "alice";  // naming self is fine
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total_count, 3);
}

TEST_F(InteractionsListTest, AdminListsAllUsersWhenUnset) {
    InteractionListFilter f;
    auto r = handler_->ListInteractions(f, RequesterContext{"root", true});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total_count, 4);  // alice 3 + bob 1
}

TEST_F(InteractionsListTest, AdminTargetsUserAndEmitsForensics) {
    InteractionListFilter f;
    f.user_id = "bob";
    testing::internal::CaptureStderr();
    auto r = handler_->ListInteractions(f, RequesterContext{"root", true});
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total_count, 1);
    EXPECT_EQ(r.value().interactions[0].user_id, "bob");
    // forensics for admin targeting bob.
    EXPECT_NE(err.find("admin_cross_user_access"), std::string::npos);
    EXPECT_NE(err.find("\"endpoint\":\"interactions\""), std::string::npos);
}

TEST_F(InteractionsListTest, FilterBySessionAndNamespace) {
    InteractionListFilter f;
    f.namespace_id = "support";
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total_count, 1);
    EXPECT_EQ(r.value().interactions[0].interaction_id, "a3");

    // sess-1 covers BOTH the base int-1 and a2 (both alice/sess-1) — DESC order: a2 then int-1.
    InteractionListFilter g;
    g.session_id = "sess-1";
    auto r2 = handler_->ListInteractions(g, RequesterContext{"alice", false});
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value().total_count, 2);
    EXPECT_EQ(r2.value().interactions[0].interaction_id, "a2");      // 05-02
    EXPECT_EQ(r2.value().interactions[1].interaction_id, "int-1");   // 05-01
}

TEST_F(InteractionsListTest, TimeRangeFilter) {
    // Compute the exact epoch-ms for 2026-05-02T12:00:00Z via timegm so the test is
    // independent of the (separately-tested) ToIso8601Utc conversion. The cutoff
    // sits between a2 (05-02 00:00) and a3 (05-03 00:00).
    std::tm tm_cut{};
    tm_cut.tm_year = 2026 - 1900; tm_cut.tm_mon = 4 /*May*/; tm_cut.tm_mday = 2;
    tm_cut.tm_hour = 12; tm_cut.tm_min = 0; tm_cut.tm_sec = 0;
    const int64_t cutoff_ms = static_cast<int64_t>(timegm(&tm_cut)) * 1000LL;

    InteractionListFilter f;
    f.from_timestamp = cutoff_ms;
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok());
    // Only a3 (05-03) qualifies for alice; int-1 (05-01) + a2 (05-02 00:00) excluded.
    ASSERT_EQ(r.value().interactions.size(), 1u);
    EXPECT_EQ(r.value().interactions[0].interaction_id, "a3");
}

TEST_F(InteractionsListTest, PaginationHasNext) {
    InteractionListFilter f;
    f.limit = 2;
    f.offset = 0;
    auto r = handler_->ListInteractions(f, RequesterContext{"root", true});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().interactions.size(), 2u);
    EXPECT_EQ(r.value().total_count, 4);
    EXPECT_TRUE(r.value().has_next);
    EXPECT_EQ(r.value().next_offset, 2);
}

TEST_F(InteractionsListTest, InvalidFilterRejected) {
    InteractionListFilter bad;
    bad.limit = 0;
    EXPECT_FALSE(handler_->ListInteractions(bad, RequesterContext{"alice", false}).ok());
    InteractionListFilter bad2;
    bad2.sort_order = "SIDEWAYS";
    auto r = handler_->ListInteractions(bad2, RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INVALID_FILTER"), std::string::npos);
}

TEST_F(InteractionsListTest, AdminCrossUserNoRowsIsEmptyNotError) {
    InteractionListFilter f;
    f.user_id = "nobody";
    auto r = handler_->ListInteractions(f, RequesterContext{"root", true});
    ASSERT_TRUE(r.ok());  // — empty, not an error
    EXPECT_EQ(r.value().total_count, 0);
    EXPECT_TRUE(r.value().interactions.empty());
}

// ---- additional validation branch coverage ----

// A negative offset is rejected (the offset < 0 validation branch; the existing
// InvalidFilterRejected only covers limit=0 and a bad sort_order).
TEST_F(InteractionsListTest, NegativeOffsetRejected) {
    InteractionListFilter f;
    f.offset = -5;
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INVALID_FILTER"), std::string::npos);
}

// limit above the cap is rejected (the limit > 200 half of the OR).
TEST_F(InteractionsListTest, LimitOverCapRejected) {
    InteractionListFilter f;
    f.limit = 999;
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INVALID_FILTER"), std::string::npos);
}

// from_timestamp > to_timestamp trips the inverted-range branch.
TEST_F(InteractionsListTest, InvertedTimestampRangeRejected) {
    InteractionListFilter f;
    f.from_timestamp = 5000;
    f.to_timestamp = 1000;
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INVALID_FILTER"), std::string::npos);
}

// An offset at/past the end of a non-empty result set is out-of-range (the
// offset > 0 && offset >= total_count branch after the count query).
TEST_F(InteractionsListTest, OffsetPastEndIsInvalidFilter) {
    InteractionListFilter f;
    f.offset = 100;  // alice has 3 rows < 100
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INVALID_FILTER"), std::string::npos);
}

// to_timestamp-only filter exercises the upper-bound predicate (created_at <= ?)
// without tripping the inverted-range guard (from has no value).
TEST_F(InteractionsListTest, ToTimestampOnlyUpperBound) {
    // alice rows: int-1 (05-01), a2 (05-02), a3 (05-03). Cap at 05-02 end → 2 rows.
    std::tm tm_utc{};
    tm_utc.tm_year = 2026 - 1900;
    tm_utc.tm_mon = 5 - 1;
    tm_utc.tm_mday = 2;
    tm_utc.tm_hour = 23;
    std::time_t secs = timegm(&tm_utc);
    InteractionListFilter f;
    f.to_timestamp = static_cast<int64_t>(secs) * 1000;
    f.sort_order = "ASC";
    auto r = handler_->ListInteractions(f, RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total_count, 2);  // 05-01 and 05-02 only
}

// An admin reading their OWN interaction takes the no-audit branch (the
// owner != requester condition is false), distinct from admin-cross-user.
TEST_F(InteractionsHandlerTest, AdminReadingOwnInteractionNoForensics) {
    Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content) "
              "VALUES('int-admin','sess-a','sales','root','user','q');");
    auto r = handler_->GetSources("int-admin", RequesterContext{"root", true});
    ASSERT_TRUE(r.ok());  // admin owns it → allowed, no forensic emit
    EXPECT_EQ(r.value().interaction_id, "int-admin");
}

// GetSources' owner-lookup prepare failure → CX_ERR_TRACE_INTERNAL. Dropping
// interaction_log makes the first SELECT fail to prepare.
TEST_F(InteractionsHandlerTest, GetSourcesOwnerLookupPrepareFailureIsInternal) {
    Exec(db_, "DROP TABLE interaction_log;");
    auto r = handler_->GetSources("int-1", RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
}

// GetSources' sources-query prepare failure → CX_ERR_TRACE_INTERNAL. The owner
// lookup succeeds (interaction_log intact) but interaction_sources is gone.
TEST_F(InteractionsHandlerTest, GetSourcesSourcesQueryPrepareFailureIsInternal) {
    Exec(db_, "DROP TABLE interaction_sources;");
    auto r = handler_->GetSources("int-1", RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
}

// ListInteractions' count-query prepare failure → CX_ERR_TRACE_INTERNAL.
TEST_F(InteractionsHandlerTest, ListCountPrepareFailureIsInternal) {
    Exec(db_, "DROP TABLE interaction_log;");
    auto r = handler_->ListInteractions(InteractionListFilter{},
                                        RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
}

// When the db has no `blocks` table, BlockExists can't probe deletions, so every
// source is treated as live (deleted_sources_count stays 0). A fresh in-memory db
// with only interaction_log + interaction_sources exercises that branch.
TEST(InteractionsHandlerNoBlocksTest, NoBlocksTableTreatsAllSourcesLive) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    auto exec = [&](const char* sql) {
        ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(db);
    };
    exec("CREATE TABLE interaction_log (id TEXT PRIMARY KEY, session_id TEXT, "
         "namespace_name TEXT, user_id TEXT, role TEXT, content TEXT, "
         "query_type TEXT, created_at TEXT);");
    exec("CREATE TABLE interaction_sources (id INTEGER PRIMARY KEY AUTOINCREMENT, "
         "interaction_id TEXT, source_block_id TEXT, source_type TEXT, "
         "relevance_score REAL, snippet TEXT);");  // no `blocks` table on purpose
    exec("INSERT INTO interaction_log(id, user_id) VALUES('ix','alice');");
    exec("INSERT INTO interaction_sources(interaction_id, source_block_id, "
         "source_type, relevance_score, snippet) VALUES('ix','bX','block',0.9,'s');");

    InteractionsHandler handler(db);
    auto r = handler.GetSources("ix", RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().deleted_sources_count, 0);  // no blocks table → assume live
    ASSERT_EQ(r.value().sources.size(), 1u);
    EXPECT_EQ(r.value().sources[0].source_block_id, "bX");
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::agent_trace
