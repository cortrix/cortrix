#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>

#include "cortrix/agent_trace/interaction_sources_schema.h"
#include "cortrix/catalog/schema_provider.h"  // ISchemaProvider + SchemaMigrator

// Branch coverage for the interaction_sources schema provider (agent trace, topic 6).
// Covers the three Migrate branches (already-current no-op, 0->1 success, exec
// failure) plus the unsupported-step error, the created table + 2 indices, the
// NOT NULL identity columns, and idempotent re-migration. interaction_sources has
// an FK to interaction_log; SQLite creates the table without the parent present
// (the FK is resolved lazily and only enforced when PRAGMA foreign_keys=ON).
namespace cortrix::agent_trace {
namespace {

std::set<std::string> QueryTextSet(sqlite3* db, const std::string& sql) {
    std::set<std::string> out;
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (t) out.insert(t);
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t QueryInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    int64_t v = -1;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

bool Contains(const std::set<std::string>& s, const std::string& k) {
    return s.find(k) != s.end();
}

// Fixture: in-memory db, the interaction_sources provider migrated once via the
// real SchemaMigrator (matches the way agent_trace_schema is exercised).
class InteractionSourcesSchemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        Status st = m.MigrateCatalog(db_);
        ASSERT_TRUE(st.ok()) << st.message();
    }
    void TearDown() override {
        if (db_) sqlite3_close(db_);
    }
    InteractionSourcesSchemaProvider provider_;
    sqlite3* db_ = nullptr;
};

TEST(InteractionSourcesSchemaProviderTest, IdentityAndVersion) {
    InteractionSourcesSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "interaction_sources");
    EXPECT_EQ(p.CurrentVersion(), 1);
    EXPECT_EQ(kInteractionSourcesSchemaVersion, 1);
}

// Already-current (1 -> 1) call is a defensive no-op (from_ver == to_ver branch).
TEST(InteractionSourcesSchemaProviderTest, AlreadyCurrentIsNoop) {
    InteractionSourcesSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());
    sqlite3_close(db);
}

// Unsupported version step (e.g. Phase 2 1 -> 2) is an error.
TEST(InteractionSourcesSchemaProviderTest, UnexpectedVersionStepIsError) {
    InteractionSourcesSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
    sqlite3_close(db);
}

// 0 -> 1 success branch invoked directly (without the migrator).
TEST(InteractionSourcesSchemaProviderTest, FreshMigrationSucceeds) {
    InteractionSourcesSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_TRUE(p.Migrate(db, 0, kInteractionSourcesSchemaVersion).ok());
    auto tables = QueryTextSet(db, "SELECT name FROM sqlite_master WHERE type='table'");
    EXPECT_TRUE(Contains(tables, "interaction_sources"));
    sqlite3_close(db);
}

// sqlite3_exec failure branch: a pre-existing index named "interaction_sources"
// defeats the "CREATE TABLE IF NOT EXISTS interaction_sources" DDL (IF NOT EXISTS
// only tolerates same-type collisions), so Migrate returns CX_ERR_TRACE_INTERNAL.
TEST(InteractionSourcesSchemaProviderTest, ExecFailureReportsInternalError) {
    InteractionSourcesSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE helper(x INTEGER)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE INDEX interaction_sources ON helper(x)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    Status st = p.Migrate(db, 0, kInteractionSourcesSchemaVersion);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
    sqlite3_close(db);
}

// The table and both §4.3 indices exist after migration.
TEST_F(InteractionSourcesSchemaTest, TableAndIndicesExist) {
    auto tables = QueryTextSet(db_, "SELECT name FROM sqlite_master WHERE type='table'");
    EXPECT_TRUE(Contains(tables, "interaction_sources"));
    auto idx = QueryTextSet(
        db_, "SELECT name FROM sqlite_master WHERE type='index' "
             "AND tbl_name='interaction_sources' AND name LIKE 'idx_sources_%'");
    EXPECT_TRUE(Contains(idx, "idx_sources_interaction"));
    EXPECT_TRUE(Contains(idx, "idx_sources_block"));
    EXPECT_EQ(idx.size(), 2u);
}

// interaction_id + source_block_id are NOT NULL; the rest are nullable.
TEST_F(InteractionSourcesSchemaTest, NotNullColumnsEnforced) {
    // A minimal row with both NOT NULL columns succeeds.
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO interaction_sources(interaction_id, source_block_id) "
        "VALUES('itx-1', 'blk-1')", nullptr, nullptr, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db_);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM interaction_sources WHERE source_type IS NULL "
        "AND relevance_score IS NULL AND snippet IS NULL"), 1);

    // Omitting NOT NULL interaction_id is rejected.
    int rc = sqlite3_exec(db_,
        "INSERT INTO interaction_sources(source_block_id) VALUES('blk-2')",
        nullptr, nullptr, nullptr);
    EXPECT_NE(rc, SQLITE_OK) << "NOT NULL interaction_id not enforced";
}

// Re-running the migration is a no-op (version-gated) and a direct DDL re-exec is
// also tolerated by IF NOT EXISTS.
TEST_F(InteractionSourcesSchemaTest, MigrationIsIdempotent) {
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO interaction_sources(interaction_id, source_block_id) "
        "VALUES('keep', 'blk')", nullptr, nullptr, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db_);

    cortrix::catalog::SchemaMigrator m2;
    m2.Register(&provider_);
    Status st = m2.MigrateCatalog(db_);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(provider_.Migrate(db_, 1, 1).ok());
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM interaction_sources"), 1);
}

}  // namespace
}  // namespace cortrix::agent_trace
