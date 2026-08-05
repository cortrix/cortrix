// Schema-provider matrices for the catalog-config / no-op providers and sparse retrieval:
//   Parser (ParserSchemaProvider, namespaces.parser_config TEXT column, version 1)
//   Reranker (RerankerSchemaProvider, reranker no-op provider, version 1)
//   Watcher (WatcherSchemaProvider, watcher no-op provider, version 1)
//   HyPE (HypeSchemaProvider, hype no-op provider, version 1, F38-specific error token)
//   Sparse retrieval (SparseSchemaProvider, sparse_inverted_index table + blocks.sparse_vec, version 1)
//
// Fresh in-memory sqlite3 per case. Globally unique suite/fixture names
// (ParserCfgMatrix / RerankerCfgMatrix / WatcherCfgMatrix / HypeCfgMatrix / SparseCfgMatrix) that do
// NOT reuse names from existing tests.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/connector/watcher_schema_provider.h"
#include "cortrix/reranker/reranker_schema_provider.h"
#include "cortrix/retrieval/sparse_schema_provider.h"
#include "cortrix/spc/hype_schema_provider.h"
#include "cortrix/spc/parser_schema_provider.h"

namespace cortrix::test_schema_matrix_config {
namespace {

constexpr const char* kNamespacesSql = R"SQL(
CREATE TABLE namespaces (
    ns_id  TEXT PRIMARY KEY,
    name   TEXT NOT NULL
);
)SQL";

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

struct SqliteHelpers {
    sqlite3* db = nullptr;
    void Open() { ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK); }
    void Close() { sqlite3_close(db); }
    int Exec(const std::string& sql) {
        return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }
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
    bool TableExists(const std::string& n) { return ObjectExists("table", n); }
    bool IndexExists(const std::string& n) { return ObjectExists("index", n); }
};

struct InitStep {
    int from;
    int to;
    bool ok;
};

// ============================ parser_config (version 1) ================================

class ParserCfgMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    void CreateNs() { ASSERT_EQ(Exec(kNamespacesSql), SQLITE_OK); }
    cortrix::spc::ParserSchemaProvider p_;
};

TEST_F(ParserCfgMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "parser");
    EXPECT_EQ(p_.CurrentVersion(), 1);
}

TEST_F(ParserCfgMatrix, MigrateAddsParserConfigColumn) {
    CreateNs();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("namespaces", "parser_config"));
    // default '{}' round-trip.
    EXPECT_EQ(Exec("INSERT INTO namespaces(ns_id, name) VALUES ('ns1','n');"), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT parser_config FROM namespaces WHERE ns_id='ns1';", -1, &stmt, nullptr);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), "{}");
    sqlite3_finalize(stmt);
}

TEST_F(ParserCfgMatrix, IdempotentReRun) {
    CreateNs();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(ParserCfgMatrix, NoNamespacesTableIsNoOp) {
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_FALSE(ColumnExists("namespaces", "parser_config"));
}

TEST_F(ParserCfgMatrix, NullDbAfterValidGate) {
    Status s = p_.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("null db"), std::string::npos);
}

class ParserCfgVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    cortrix::spc::ParserSchemaProvider p_;
};
TEST_P(ParserCfgVersionMatrix, Gate) {
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
    ParserCfgSteps, ParserCfgVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{7, 7, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ reranker no-op (version 1) ================================

class RerankerCfgMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::reranker::RerankerSchemaProvider p_;
};

TEST_F(RerankerCfgMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "reranker");
    EXPECT_EQ(p_.CurrentVersion(), 1);
}

TEST_F(RerankerCfgMatrix, InitAndNoOpWithRealAndNullDb) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());     // init no-op
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());      // already-current
    EXPECT_TRUE(p_.Migrate(nullptr, 0, 1).ok()); // ignores db entirely
    EXPECT_TRUE(p_.Migrate(nullptr, 5, 5).ok()); // any n->n
}

TEST_F(RerankerCfgMatrix, UnsupportedStepRejected) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("reranker"), std::string::npos);
}

class RerankerCfgVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    cortrix::reranker::RerankerSchemaProvider p_;
};
TEST_P(RerankerCfgVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(nullptr, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    RerankerCfgSteps, RerankerCfgVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{9, 9, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ watcher no-op (version 1) ================================

class WatcherCfgMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::connector::WatcherSchemaProvider p_;
};

TEST_F(WatcherCfgMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "watcher");
    EXPECT_EQ(p_.CurrentVersion(), 1);
}

TEST_F(WatcherCfgMatrix, InitAndNoOp) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
    EXPECT_TRUE(p_.Migrate(nullptr, 0, 1).ok());
}

TEST_F(WatcherCfgMatrix, UnsupportedStepRejected) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("watcher"), std::string::npos);
}

class WatcherCfgVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    cortrix::connector::WatcherSchemaProvider p_;
};
TEST_P(WatcherCfgVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(nullptr, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    WatcherCfgSteps, WatcherCfgVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{3, 3, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ hype no-op (version 1, HyPE token) ===============================

class HypeCfgMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::spc::HypeSchemaProvider p_;
};

TEST_F(HypeCfgMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "hype");
    EXPECT_EQ(p_.CurrentVersion(), 1);
}

TEST_F(HypeCfgMatrix, InitAndNoOp) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
    EXPECT_TRUE(p_.Migrate(nullptr, 0, 1).ok());
}

TEST_F(HypeCfgMatrix, UnsupportedStepRejectedWithHypeToken) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    // HyPE uses the feature-scoped token CX_ERR_HYPE_SCHEMA_VERSION_MISMATCH.
    EXPECT_NE(s.message().find("CX_ERR_HYPE_SCHEMA_VERSION_MISMATCH"), std::string::npos);
}

class HypeCfgVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    cortrix::spc::HypeSchemaProvider p_;
};
TEST_P(HypeCfgVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(nullptr, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_HYPE_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    HypeCfgSteps, HypeCfgVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{6, 6, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ sparse inverted index (version 1) ================================

class SparseCfgMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    void CreateBlocks() { ASSERT_EQ(Exec(kBlocksSql), SQLITE_OK); }
    cortrix::retrieval::SparseSchemaProvider p_;
};

TEST_F(SparseCfgMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "sparse_index");
    EXPECT_EQ(p_.CurrentVersion(), 1);
}

TEST_F(SparseCfgMatrix, MigrateCreatesInvertedIndexTableAndIndexes) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("sparse_inverted_index"));
    EXPECT_TRUE(IndexExists("idx_term_lookup"));
    EXPECT_TRUE(IndexExists("idx_child_lookup"));
    // blocks absent → sparse_vec ALTER skipped, inverted index still created.
    EXPECT_FALSE(ColumnExists("blocks", "sparse_vec"));
}

TEST_F(SparseCfgMatrix, InvertedIndexShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("sparse_inverted_index", "ns_id"));
    EXPECT_TRUE(ColumnExists("sparse_inverted_index", "term_id"));
    EXPECT_TRUE(ColumnExists("sparse_inverted_index", "child_id"));
    EXPECT_TRUE(ColumnExists("sparse_inverted_index", "weight"));
}

TEST_F(SparseCfgMatrix, AddsBlocksSparseVecWhenBlocksPresent) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("blocks", "sparse_vec"));
    EXPECT_TRUE(TableExists("sparse_inverted_index"));
}

TEST_F(SparseCfgMatrix, IdempotentReRun) {
    CreateBlocks();
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(SparseCfgMatrix, NullDbRejectedWithMismatchToken) {
    // Sparse retrieval returns the SCHEMA_VERSION_MISMATCH token for a null db on the 0->1 path.
    Status s = p_.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
}

TEST_F(SparseCfgMatrix, FloatWeightRoundTrip) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_EQ(Exec("INSERT INTO sparse_inverted_index(ns_id, term_id, child_id, weight) "
                   "VALUES ('ns1', 7, 'c1', 0.25);"), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT weight FROM sparse_inverted_index WHERE child_id='c1';", -1,
                       &stmt, nullptr);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_FLOAT_EQ(static_cast<float>(sqlite3_column_double(stmt, 0)), 0.25f);
    sqlite3_finalize(stmt);
}

class SparseCfgVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::retrieval::SparseSchemaProvider p_;
};
TEST_P(SparseCfgVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    }
}
INSTANTIATE_TEST_SUITE_P(
    SparseCfgSteps, SparseCfgVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{4, 4, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

}  // namespace
}  // namespace cortrix::test_schema_matrix_config
