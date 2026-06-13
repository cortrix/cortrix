// F38 S0 — kBlockHypeQuestion=16 block sub-type + F38SchemaProvider. Unlike F03
// (real per-Unit columns), F38 owns no table/column: a hype_question Block is a
// row in the F09 blocks table with block_type=16, so Migrate(0->1) is a no-op.
// These tests pin the enum value, the no-op semantics, and the SchemaMigrator
// registration closure. Mirrors test_reranker_schema_provider / F03.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/block_types.h"
#include "cortrix/spc/f38_schema_provider.h"

namespace cortrix::spc {
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

// ---------- block_type enum ----------

TEST(F38BlockTypeTest, HypeQuestionIs16) {
    // F38 owns block sub-type 16 (F09 §4.4 authorized). Pin the value so a later
    // edit to the shared enum that moved it would fail here.
    EXPECT_EQ(static_cast<uint16_t>(cortrix::kBlockHypeQuestion), 16);
}

TEST(F38BlockTypeTest, DoesNotCollideWithExistingTypes) {
    // 16 must differ from every pre-existing sub-type (1-8) and from F08's meta.
    EXPECT_NE(cortrix::kBlockHypeQuestion, cortrix::kBlockMeta);     // 8
    EXPECT_NE(cortrix::kBlockHypeQuestion, cortrix::kBlockFile);     // 1
    EXPECT_NE(cortrix::kBlockHypeQuestion, cortrix::kBlockMemory);   // 7
    // F41 owns 17 — F38 must not have taken it.
    EXPECT_NE(static_cast<uint16_t>(cortrix::kBlockHypeQuestion), 17);
}

// ---------- F38SchemaProvider ----------

TEST(F38SchemaProviderTest, IdentityAndVersion) {
    F38SchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "F38");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(F38SchemaProviderTest, MigrateIsNoOpNoNewTable) {
    F38SchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    auto before = TableNames(db);
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());  // init no-op
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());  // already current
    auto after = TableNames(db);
    EXPECT_EQ(before, after);  // F38 creates no table (contrast F03)
    sqlite3_close(db);
}

TEST(F38SchemaProviderTest, UnexpectedVersionStepIsError) {
    F38SchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);  // Phase 2 not implemented
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_F38_SCHEMA_VERSION_MISMATCH"),
              std::string::npos);
    sqlite3_close(db);
}

TEST(F38SchemaProviderTest, RegistersWithSchemaMigrator) {
    // F38 provider runs inside the F12 SchemaMigrator (the D3.5 integrated path).
    // Pin the closure: MigrateUnit succeeds and records F38 at version 1 without
    // creating any F38 table.
    F38SchemaProvider p;
    cortrix::catalog::SchemaMigrator migrator;
    migrator.Register(&p);
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    auto before = TableNames(db);
    ASSERT_TRUE(migrator.MigrateUnit(db, "unit_test").ok());
    EXPECT_EQ(migrator.CurrentVersion(db, "F38"), 1);
    // Only the migrator's own bookkeeping table may appear; no F38 table.
    auto after = TableNames(db);
    EXPECT_EQ(after.count("hype_questions"), 0u);
    EXPECT_EQ(after.count("hype"), 0u);
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::spc
