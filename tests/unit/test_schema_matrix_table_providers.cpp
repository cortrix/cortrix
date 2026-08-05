// Schema-provider matrices for the table-creating providers:
//   Block header (BlockFrameworkSchemaProvider, per-Unit framework: documents/blocks/blocks_fts + idx + triggers)
//   META block (MetadataSchemaProvider, metadata_blocks table, version 1)
//   DB import (ImportSchemaProvider, db_connections + import_tasks, version 1; FK -> tenants/namespaces)
//   Doc summary (DocSummarySchemaProvider, doc_fts5_index FTS5 vtable, version 1; doc-summary-scoped error token)
//
// Fresh in-memory sqlite3 per case. Globally unique suite/fixture names
// (BlockFrameworkTblMatrix / MetadataTblMatrix / ImportTblMatrix / DocSummaryTblMatrix) that do NOT reuse the
// names in test_import_schema_provider.cpp / test_metadata_schema_provider.cpp etc.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/doc_summary/doc_summary_schema_provider.h"
#include "cortrix/import/import_schema_provider.h"
#include "cortrix/metadata/metadata_schema_provider.h"
#include "cortrix/store/block_framework_schema_provider.h"

namespace cortrix::test_schema_matrix_table {
namespace {

// FK targets for DB import (catalog.db TEXT PKs). Created before DB import's Migrate runs.
constexpr const char* kCatalogFkSql = R"SQL(
CREATE TABLE tenants    (tenant_id TEXT PRIMARY KEY, name TEXT);
CREATE TABLE namespaces (ns_id TEXT PRIMARY KEY, name TEXT);
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
    bool TriggerExists(const std::string& n) { return ObjectExists("trigger", n); }
};

struct InitStep {
    int from;
    int to;
    bool ok;
};

// ============================ block header per-Unit framework (version 1) ===================

class BlockFrameworkTblMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::store::BlockFrameworkSchemaProvider p_;
};

TEST_F(BlockFrameworkTblMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "block_framework");
    EXPECT_EQ(p_.CurrentVersion(), 1);
}

TEST_F(BlockFrameworkTblMatrix, MigrateCreatesCoreTables) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("documents"));
    EXPECT_TRUE(TableExists("blocks"));
    EXPECT_TRUE(TableExists("blocks_fts"));
}

TEST_F(BlockFrameworkTblMatrix, MigrateCreatesIndexesAndTriggers) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(IndexExists("idx_doc_source"));
    EXPECT_TRUE(IndexExists("idx_doc_status"));
    EXPECT_TRUE(IndexExists("idx_block_doc"));
    EXPECT_TRUE(IndexExists("idx_block_type"));
    EXPECT_TRUE(TriggerExists("blocks_ai"));
    EXPECT_TRUE(TriggerExists("blocks_ad"));
    EXPECT_TRUE(TriggerExists("blocks_au"));
}

TEST_F(BlockFrameworkTblMatrix, DocumentsAndBlocksShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("documents", "doc_id"));
    EXPECT_TRUE(ColumnExists("documents", "source_type"));
    EXPECT_TRUE(ColumnExists("documents", "deleted_at"));
    EXPECT_TRUE(ColumnExists("blocks", "block_id"));
    EXPECT_TRUE(ColumnExists("blocks", "block_type"));
    EXPECT_TRUE(ColumnExists("blocks", "content_text"));
}

TEST_F(BlockFrameworkTblMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(BlockFrameworkTblMatrix, UnsupportedStepRejected) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("block_framework"), std::string::npos);
}

class BlockFrameworkTblVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::store::BlockFrameworkSchemaProvider p_;
};
TEST_P(BlockFrameworkTblVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    BlockFrameworkTblSteps, BlockFrameworkTblVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{5, 5, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ META block metadata_blocks (version 1) =====================

class MetadataTblMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::metadata::MetadataSchemaProvider p_;
};

TEST_F(MetadataTblMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "metadata_block");
    EXPECT_EQ(p_.CurrentVersion(), 1);
    EXPECT_EQ(cortrix::metadata::kMetadataSchemaVersion, 1);
}

TEST_F(MetadataTblMatrix, MigrateCreatesTableAndIndex) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("metadata_blocks"));
    EXPECT_TRUE(IndexExists("idx_metablocks_ns"));
}

TEST_F(MetadataTblMatrix, TableShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("metadata_blocks", "block_id"));
    EXPECT_TRUE(ColumnExists("metadata_blocks", "doc_id"));
    EXPECT_TRUE(ColumnExists("metadata_blocks", "namespace_id"));
    EXPECT_TRUE(ColumnExists("metadata_blocks", "block_text"));
    EXPECT_TRUE(ColumnExists("metadata_blocks", "metadata_json"));
    EXPECT_TRUE(ColumnExists("metadata_blocks", "created_at"));
}

TEST_F(MetadataTblMatrix, DocIdUniqueConstraint) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_EQ(Exec("INSERT INTO metadata_blocks(block_id, doc_id, namespace_id, block_text, "
                   "metadata_json) VALUES ('b1','d1','ns1','t','{}');"), SQLITE_OK);
    // Second row with the same doc_id violates UNIQUE.
    EXPECT_NE(Exec("INSERT INTO metadata_blocks(block_id, doc_id, namespace_id, block_text, "
                   "metadata_json) VALUES ('b2','d1','ns1','t','{}');"), SQLITE_OK);
}

TEST_F(MetadataTblMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(MetadataTblMatrix, UnsupportedStepRejected) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("metadata_block"), std::string::npos);
}

class MetadataTblVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::metadata::MetadataSchemaProvider p_;
};
TEST_P(MetadataTblVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    MetadataTblSteps, MetadataTblVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{8, 8, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ db_connections + import_tasks (version 1) =================================

class ImportTblMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override {
        Open();
        ASSERT_EQ(Exec(kCatalogFkSql), SQLITE_OK);  // tenants/namespaces FK targets
    }
    void TearDown() override { Close(); }
    cortrix::import::ImportSchemaProvider p_;
};

TEST_F(ImportTblMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "db_import");
    EXPECT_EQ(p_.CurrentVersion(), 1);
    EXPECT_EQ(cortrix::import::kImportSchemaVersion, 1);
}

TEST_F(ImportTblMatrix, MigrateCreatesBothTables) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("db_connections"));
    EXPECT_TRUE(TableExists("import_tasks"));
}

TEST_F(ImportTblMatrix, MigrateCreatesIndexes) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(IndexExists("idx_db_connections_tenant"));
    EXPECT_TRUE(IndexExists("idx_db_connections_active"));
    EXPECT_TRUE(IndexExists("idx_import_tasks_ns_status"));
    EXPECT_TRUE(IndexExists("idx_import_tasks_running"));
}

TEST_F(ImportTblMatrix, DbConnectionsShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("db_connections", "ref_id"));
    EXPECT_TRUE(ColumnExists("db_connections", "tenant_id"));
    EXPECT_TRUE(ColumnExists("db_connections", "secret_key_id"));
    EXPECT_TRUE(ColumnExists("db_connections", "expires_at"));
    EXPECT_TRUE(ColumnExists("db_connections", "revoked_at"));
}

TEST_F(ImportTblMatrix, ImportTasksShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("import_tasks", "task_id"));
    EXPECT_TRUE(ColumnExists("import_tasks", "ns_id"));
    EXPECT_TRUE(ColumnExists("import_tasks", "status"));
    EXPECT_TRUE(ColumnExists("import_tasks", "progress"));
    EXPECT_TRUE(ColumnExists("import_tasks", "error_code"));
}

TEST_F(ImportTblMatrix, ImportTasksDefaultProgressFloat) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    ASSERT_EQ(Exec("INSERT INTO tenants(tenant_id, name) VALUES ('t1','t');"), SQLITE_OK);
    ASSERT_EQ(Exec("INSERT INTO namespaces(ns_id, name) VALUES ('ns1','n');"), SQLITE_OK);
    ASSERT_EQ(Exec("INSERT INTO import_tasks(task_id, ns_id, tenant_id, connection_ref_id, "
                   "request_json, text_strategy, submitted_by) "
                   "VALUES ('imp1','ns1','t1','r1','{}','per_row','u1');"), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT progress FROM import_tasks WHERE task_id='imp1';", -1, &stmt,
                       nullptr);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_FLOAT_EQ(static_cast<float>(sqlite3_column_double(stmt, 0)), 0.0f);
    sqlite3_finalize(stmt);
}

TEST_F(ImportTblMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(ImportTblMatrix, UnsupportedStepRejected) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("db_import"), std::string::npos);
}

class ImportTblVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db_, kCatalogFkSql, nullptr, nullptr, nullptr), SQLITE_OK);
    }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::import::ImportSchemaProvider p_;
};
TEST_P(ImportTblVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    ImportTblSteps, ImportTblVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{6, 6, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ doc_fts5_index (version 1, doc summary token) ========================

class DocSummaryTblMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::doc_summary::DocSummarySchemaProvider p_;
};

TEST_F(DocSummaryTblMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "doc_summary");
    EXPECT_EQ(p_.CurrentVersion(), 1);
}

TEST_F(DocSummaryTblMatrix, MigrateCreatesFtsVTable) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("doc_fts5_index"));
    EXPECT_TRUE(ColumnExists("doc_fts5_index", "filename"));
    EXPECT_TRUE(ColumnExists("doc_fts5_index", "doc_title"));
    EXPECT_TRUE(ColumnExists("doc_fts5_index", "authors"));
}

TEST_F(DocSummaryTblMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(DocSummaryTblMatrix, NullDbRejectedWithDocSummaryToken) {
    Status s = p_.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_DOCSUMMARY_SCHEMA_VERSION_MISMATCH"), std::string::npos);
}

TEST_F(DocSummaryTblMatrix, UnsupportedStepRejectedWithDocSummaryToken) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_DOCSUMMARY_SCHEMA_VERSION_MISMATCH"), std::string::npos);
}

class DocSummaryTblVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::doc_summary::DocSummarySchemaProvider p_;
};
TEST_P(DocSummaryTblVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_DOCSUMMARY_SCHEMA_VERSION_MISMATCH"), std::string::npos);
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    }
}
INSTANTIATE_TEST_SUITE_P(
    DocSummaryTblSteps, DocSummaryTblVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{4, 4, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

}  // namespace
}  // namespace cortrix::test_schema_matrix_table
