// Schema-provider matrices for the per-Unit `blocks`-altering enrichment providers:
//   Enricher (EnricherSchemaProvider, enriched_score + entities + FTS5, version 2, forward-only)
//   Semantic score (ScoringSchemaProvider, semantic_score, version 1)
//   Contextual retrieval (ContextualSchemaProvider, +4 contextual-retrieval columns, version 1)
//   Parent-child chunking (ParentChildSchemaProvider, parents table + 4 child columns + 3 indexes, version 1)
//
// Each provider: feature identity, Migrate(0->1) shape, idempotent re-run, unsupported
// version step rejected with the real CX_ERR token, null-db handling, and the
// no-blocks-table no-op guard. Fresh in-memory sqlite3 per case. Suite/fixture names are
// globally unique (EnricherBlkMatrix / ScoringBlkMatrix / ContextualBlkMatrix / ParentChildBlkMatrix) and do NOT
// reuse the names in test_enricher_schema_provider.cpp / test_scoring_schema_provider.cpp.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/scoring/scoring_schema_provider.h"
#include "cortrix/spc_enricher/enricher_schema_provider.h"
#include "cortrix/spc_enricher/contextual_schema_provider.h"
#include "cortrix/store/parent_child_schema_provider.h"

namespace cortrix::test_schema_matrix_blocks {
namespace {

// Minimal block-header-owned per-Unit `blocks` stand-in: enough shape to ALTER + index.
constexpr const char* kBlocksSql = R"SQL(
CREATE TABLE blocks (
    block_id         INTEGER PRIMARY KEY,
    doc_id           INTEGER NOT NULL,
    chunk_index      INTEGER NOT NULL,
    block_type       INTEGER NOT NULL,
    processing_level INTEGER NOT NULL,
    data             BLOB NOT NULL
);
)SQL";

// Shared sqlite helpers used by every fixture in this file.
struct SqliteHelpers {
    sqlite3* db = nullptr;

    void Open() { ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK); }
    void Close() { sqlite3_close(db); }

    int Exec(const std::string& sql) {
        return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }
    void CreateBlocks() { ASSERT_EQ(Exec(kBlocksSql), SQLITE_OK); }

    bool ColumnExists(const char* table, const char* column) {
        std::string sql = std::string("SELECT 1 FROM pragma_table_info('") + table +
                          "') WHERE name='" + column + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }
    bool ObjectExists(const char* type, const std::string& name) {
        std::string sql = std::string("SELECT 1 FROM sqlite_master WHERE type='") + type +
                          "' AND name='" + name + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }
    bool IndexExists(const std::string& name) { return ObjectExists("index", name); }
    bool TableExists(const std::string& name) { return ObjectExists("table", name); }
    int ColumnCount(const char* table) {
        std::string sql = std::string("SELECT COUNT(*) FROM pragma_table_info('") + table + "');";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        int n = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    }
};

// ============================ enricher (version 2, forward-only) =======================

class EnricherBlkMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::spc::EnricherSchemaProvider p_;
};

TEST_F(EnricherBlkMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "enricher");
    EXPECT_EQ(p_.CurrentVersion(), 2);
}

TEST_F(EnricherBlkMatrix, MigrateAddsEnrichmentColumns) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("blocks", "enriched_score"));
    EXPECT_TRUE(ColumnExists("blocks", "enriched_at"));
    EXPECT_TRUE(ColumnExists("blocks", "enricher_metadata"));
}

TEST_F(EnricherBlkMatrix, MigrateCreatesEntitiesTableAndIndexes) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("entities"));
    EXPECT_TRUE(IndexExists("idx_entities_block"));
    EXPECT_TRUE(IndexExists("idx_entities_type"));
    EXPECT_TRUE(IndexExists("idx_blocks_enriched_score"));
}

TEST_F(EnricherBlkMatrix, MigrateCreatesEntitiesFts) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("entities_fts"));
}

TEST_F(EnricherBlkMatrix, EntitiesShapeAndFloatRoundTrip) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    // entities has 6 columns (entity_id, block_id, text, type, start_offset, end_offset).
    EXPECT_EQ(ColumnCount("entities"), 6);
    EXPECT_EQ(Exec("INSERT INTO blocks(block_id, doc_id, chunk_index, block_type, "
                   "processing_level, data) VALUES (1, 1, 0, 1, 2, x'00');"), SQLITE_OK);
    EXPECT_EQ(Exec("UPDATE blocks SET enriched_score=0.75 WHERE block_id=1;"), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT enriched_score FROM blocks WHERE block_id=1;", -1, &stmt, nullptr);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_FLOAT_EQ(static_cast<float>(sqlite3_column_double(stmt, 0)), 0.75f);
    sqlite3_finalize(stmt);
}

TEST_F(EnricherBlkMatrix, IdempotentReRun) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());  // re-run 0->1 no-op
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());  // already-current no-op
    EXPECT_TRUE(p_.Migrate(db, 2, 2).ok());  // current no-op
}

TEST_F(EnricherBlkMatrix, ForwardOneToTwoBackfill) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    // 1->2 is a valid forward backfill (entities FK cascade rebuild path).
    EXPECT_TRUE(p_.Migrate(db, 1, 2).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 2).ok());
}

TEST_F(EnricherBlkMatrix, NoBlocksTableIsNoOp) {
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_FALSE(ColumnExists("blocks", "enriched_score"));
    EXPECT_FALSE(TableExists("entities"));
}

TEST_F(EnricherBlkMatrix, NullDbAfterValidGate) {
    Status s = p_.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("null db"), std::string::npos);
}

// Forward-only version gate matrix: only steps with 0<=from<=to<=2 are accepted.
struct EnricherStep {
    int from;
    int to;
    bool ok;
};
class EnricherBlkVersionMatrix : public ::testing::TestWithParam<EnricherStep> {
protected:
    cortrix::spc::EnricherSchemaProvider p_;
};
TEST_P(EnricherBlkVersionMatrix, GateOnlyForwardWithinCurrent) {
    const EnricherStep step = GetParam();
    // Null db so we exercise ONLY the version gate (gate runs before the null check
    // for valid steps; for invalid steps the gate rejects regardless of db).
    Status s = p_.Migrate(nullptr, step.from, step.to);
    if (step.ok) {
        // Valid forward step → passes gate, then hits the null-db guard.
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
        EXPECT_NE(s.message().find("null db"), std::string::npos);
    } else {
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    EnricherBlkSteps, EnricherBlkVersionMatrix,
    ::testing::Values(
        EnricherStep{0, 1, true}, EnricherStep{0, 2, true}, EnricherStep{1, 2, true},
        EnricherStep{2, 2, true}, EnricherStep{1, 1, true}, EnricherStep{0, 0, true},
        EnricherStep{2, 3, false}, EnricherStep{0, 3, false}, EnricherStep{1, 3, false},
        EnricherStep{2, 1, false}, EnricherStep{2, 0, false}, EnricherStep{1, 0, false},
        EnricherStep{-1, 1, false}, EnricherStep{0, 5, false}, EnricherStep{3, 4, false}));

// ============================ semantic score scoring (version 1) =================

class ScoringBlkMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::scoring::ScoringSchemaProvider p_;
};

TEST_F(ScoringBlkMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "scoring");
    EXPECT_EQ(p_.CurrentVersion(), 1);
    EXPECT_EQ(cortrix::scoring::kScoringSchemaVersion, 1);
}

TEST_F(ScoringBlkMatrix, MigrateAddsColumnAndPartialIndex) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("blocks", "semantic_score"));
    EXPECT_TRUE(IndexExists("idx_blocks_semantic_score"));
}

TEST_F(ScoringBlkMatrix, IdempotentReRun) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(ScoringBlkMatrix, NoBlocksTableIsNoOp) {
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_FALSE(ColumnExists("blocks", "semantic_score"));
}

TEST_F(ScoringBlkMatrix, NullDbAfterValidGate) {
    Status s = p_.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("null db"), std::string::npos);
}

TEST_F(ScoringBlkMatrix, FloatRoundTrip) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_EQ(Exec("INSERT INTO blocks(block_id, doc_id, chunk_index, block_type, "
                   "processing_level, data) VALUES (1, 1, 0, 1, 2, x'00');"), SQLITE_OK);
    EXPECT_EQ(Exec("UPDATE blocks SET semantic_score=0.4 WHERE block_id=1;"), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT semantic_score FROM blocks WHERE block_id=1;", -1, &stmt, nullptr);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_FLOAT_EQ(static_cast<float>(sqlite3_column_double(stmt, 0)), 0.4f);
    sqlite3_finalize(stmt);
}

struct InitStep {
    int from;
    int to;
    bool ok;  // true = pass gate (0->1 or n->n)
};
class ScoringBlkVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    cortrix::scoring::ScoringSchemaProvider p_;
};
TEST_P(ScoringBlkVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(nullptr, step.from, step.to);
    if (step.ok) {
        EXPECT_NE(s.message().find("null db"), std::string::npos);
    } else {
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}
INSTANTIATE_TEST_SUITE_P(
    ScoringBlkSteps, ScoringBlkVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{3, 5, false}));

// ============================ contextual retrieval (version 1, +4 columns) ===========

class ContextualBlkMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::spc::ContextualSchemaProvider p_;
};

TEST_F(ContextualBlkMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "contextual");
    EXPECT_EQ(p_.CurrentVersion(), 2);  // V2 = + contextual_vec_labels (§3.8 W2)
}

TEST_F(ContextualBlkMatrix, MigrateAddsFourColumns) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("blocks", "embedding"));
    EXPECT_TRUE(ColumnExists("blocks", "contextualized_text"));
    EXPECT_TRUE(ColumnExists("blocks", "contextualized_embedding"));
    EXPECT_TRUE(ColumnExists("blocks", "contextualized_status"));
}

TEST_F(ContextualBlkMatrix, IdempotentReRun) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(ContextualBlkMatrix, NoBlocksTableIsNoOp) {
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_FALSE(ColumnExists("blocks", "embedding"));
}

TEST_F(ContextualBlkMatrix, NullDbAfterValidGate) {
    Status s = p_.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("null db"), std::string::npos);
}

TEST_F(ContextualBlkMatrix, CoexistsWithEnricherColumns) {
    CreateBlocks();
    ASSERT_EQ(Exec("ALTER TABLE blocks ADD COLUMN enriched_score REAL DEFAULT NULL;"), SQLITE_OK);
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("blocks", "enriched_score"));
    EXPECT_TRUE(ColumnExists("blocks", "embedding"));
}

class ContextualBlkVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    cortrix::spc::ContextualSchemaProvider p_;
};
TEST_P(ContextualBlkVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(nullptr, step.from, step.to);
    if (step.ok) {
        EXPECT_NE(s.message().find("null db"), std::string::npos);
    } else {
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}
// V2 gate contract (forward-only up to CurrentVersion()==2, enricher-style): any
// 0 ≤ from ≤ to ≤ 2 passes the version gate (then trips the null-db arm here);
// backward steps and beyond-current versions (incl. same-version pairs > 2,
// formerly tolerated) are CX_ERR_SCHEMA_VERSION_MISMATCH.
INSTANTIATE_TEST_SUITE_P(
    ContextualBlkSteps, ContextualBlkVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, true}, InitStep{0, 2, true},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{4, 4, false},
                      InitStep{3, 3, false}, InitStep{10, 10, false}, InitStep{0, 3, false},
                      InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false},
                      InitStep{5, 1, false}));

// ============================ parent-child chunking (version 1, parents + child cols) ==========

class ParentChildBlkMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::store::ParentChildSchemaProvider p_;
};

TEST_F(ParentChildBlkMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "parent_child");
    EXPECT_EQ(p_.CurrentVersion(), 1);
}

TEST_F(ParentChildBlkMatrix, CreatesParentsTableEvenWithoutBlocks) {
    // parents is created unconditionally (independent of blocks).
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("parents"));
    EXPECT_TRUE(IndexExists("idx_parents_doc"));
    EXPECT_TRUE(IndexExists("idx_parents_ns"));
    // blocks ALTERs are skipped (no blocks table).
    EXPECT_FALSE(ColumnExists("blocks", "child_id"));
}

TEST_F(ParentChildBlkMatrix, MigrateAddsChildColumnsAndIndexes) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("blocks", "child_id"));
    EXPECT_TRUE(ColumnExists("blocks", "parent_id"));
    EXPECT_TRUE(ColumnExists("blocks", "token_count"));
    EXPECT_TRUE(ColumnExists("blocks", "parent_offset"));
    EXPECT_TRUE(IndexExists("idx_blocks_child_id"));
    EXPECT_TRUE(IndexExists("idx_blocks_parent"));
    EXPECT_TRUE(IndexExists("idx_blocks_meta_doc"));
}

TEST_F(ParentChildBlkMatrix, ParentsShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("parents", "parent_id"));
    EXPECT_TRUE(ColumnExists("parents", "parent_text"));
    EXPECT_TRUE(ColumnExists("parents", "hotness_score"));
}

TEST_F(ParentChildBlkMatrix, IdempotentReRun) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(ParentChildBlkMatrix, NullDbAfterValidGate) {
    Status s = p_.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("null db"), std::string::npos);
}

class ParentChildBlkVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    cortrix::store::ParentChildSchemaProvider p_;
};
TEST_P(ParentChildBlkVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(nullptr, step.from, step.to);
    if (step.ok) {
        EXPECT_NE(s.message().find("null db"), std::string::npos);
    } else {
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}
INSTANTIATE_TEST_SUITE_P(
    ParentChildBlkSteps, ParentChildBlkVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{5, 5, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

}  // namespace
}  // namespace cortrix::test_schema_matrix_blocks
