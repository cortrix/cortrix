// F40 S5 — F40SchemaProvider creates the net-new sparse_inverted_index table
// inside the F12 SchemaMigrator framework. Unlike F02 (no-op), F40's
// Migrate(0→1) emits real DDL — these tests pin identity/version, the table +
// index existence after migration, idempotency, and the bad-version-step guard.
// Mirrors test_reranker_schema_provider.cpp.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/retrieval/f40_schema_provider.h"

namespace cortrix::retrieval {
namespace {

std::set<std::string> TableNames(sqlite3* db) {
    std::set<std::string> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table'", -1, &st,
            nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            if (t) out.insert(t);
        }
    }
    sqlite3_finalize(st);
    return out;
}

std::set<std::string> IndexNames(sqlite3* db) {
    std::set<std::string> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='index'", -1, &st,
            nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            if (t) out.insert(t);
        }
    }
    sqlite3_finalize(st);
    return out;
}

std::set<std::string> ColumnNames(sqlite3* db, const std::string& table) {
    std::set<std::string> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = "SELECT name FROM pragma_table_info('" + table + "')";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            if (t) out.insert(t);
        }
    }
    sqlite3_finalize(st);
    return out;
}

TEST(F40SchemaProviderTest, IdentityAndVersion) {
    F40SchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "F40");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(F40SchemaProviderTest, MigrateCreatesTableAndIndexes) {
    F40SchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    EXPECT_TRUE(TableNames(db).count("sparse_inverted_index"));
    auto idx = IndexNames(db);
    EXPECT_TRUE(idx.count("idx_term_lookup"));
    EXPECT_TRUE(idx.count("idx_child_lookup"));

    auto cols = ColumnNames(db, "sparse_inverted_index");
    EXPECT_TRUE(cols.count("ns_id"));
    EXPECT_TRUE(cols.count("term_id"));
    EXPECT_TRUE(cols.count("child_id"));
    EXPECT_TRUE(cols.count("weight"));
    sqlite3_close(db);
}

TEST(F40SchemaProviderTest, MigrateAddsSparseVecToBlocks) {
    // [A unified-blocks] when the F09 framework `blocks` table is present, F40 also
    // ALTERs it to add the sparse_vec BLOB column (child rows' SPLADE vector).
    F40SchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "doc_id TEXT, block_type INTEGER, child_id TEXT)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_FALSE(ColumnNames(db, "blocks").count("sparse_vec"));

    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    EXPECT_TRUE(ColumnNames(db, "blocks").count("sparse_vec"));
    EXPECT_TRUE(TableNames(db).count("sparse_inverted_index"));  // both done in one migrate
    // Idempotent: re-running must not error on the already-present column.
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    sqlite3_close(db);
}

TEST(F40SchemaProviderTest, MigrateIsIdempotent) {
    F40SchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());  // re-run → IF NOT EXISTS, ok
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());  // already current
    EXPECT_TRUE(TableNames(db).count("sparse_inverted_index"));
    sqlite3_close(db);
}

TEST(F40SchemaProviderTest, UnexpectedVersionStepIsError) {
    F40SchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);  // Phase 2 not implemented
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"),
              std::string::npos);
    sqlite3_close(db);
}

TEST(F40SchemaProviderTest, NullDbRejected) {
    F40SchemaProvider p;
    Status st = p.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(st.ok());
}

TEST(F40SchemaProviderTest, RegistersWithSchemaMigrator) {
    // F40 provider runs inside the F12 SchemaMigrator transaction (the D3.5
    // integrated path). Pin the closure: after MigrateUnit the table exists.
    F40SchemaProvider p;
    cortrix::catalog::SchemaMigrator migrator;
    migrator.Register(&p);
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_TRUE(migrator.MigrateUnit(db, "unit_test").ok());
    EXPECT_TRUE(TableNames(db).count("sparse_inverted_index"));
    EXPECT_EQ(migrator.CurrentVersion(db, "F40"), 1);
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::retrieval
