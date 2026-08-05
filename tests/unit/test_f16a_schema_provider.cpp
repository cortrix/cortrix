#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/import/f16a_schema_provider.h"

// S1 coverage: F16aSchemaProvider creates db_connections + import_tasks + their
// indices (DB import / §4.2) in the catalog DB, idempotently, inside the frozen
// ISchemaProvider contract.
namespace cortrix::import {
namespace {

// Minimal stand-ins for the catalog FK targets (catalog tenants / namespaces). The real
// catalog migration creates the full tables; we only need the PKs the DB import FKs
// reference so foreign_keys=ON is satisfiable.
constexpr const char* kPrereqSql = R"SQL(
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS tenants (tenant_id TEXT PRIMARY KEY);
CREATE TABLE IF NOT EXISTS namespaces (ns_id TEXT PRIMARY KEY, tenant_id TEXT);
INSERT OR IGNORE INTO tenants(tenant_id) VALUES ('t1');
INSERT OR IGNORE INTO namespaces(ns_id, tenant_id) VALUES ('customer_kb', 't1');
)SQL";

class F16aSchemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(db_, kPrereqSql, nullptr, nullptr, &err), SQLITE_OK)
            << (err ? err : "");
        sqlite3_free(err);
    }
    void TearDown() override { sqlite3_close(db_); }

    bool TableExists(const std::string& name) {
        std::string sql =
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='" + name + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }

    bool IndexExists(const std::string& name) {
        std::string sql =
            "SELECT 1 FROM sqlite_master WHERE type='index' AND name='" + name + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }

    sqlite3* db_ = nullptr;
};

TEST_F(F16aSchemaTest, MetadataMatchesFrozenInterface) {
    F16aSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "F16a");
    EXPECT_EQ(p.CurrentVersion(), 1);
    EXPECT_EQ(kF16aSchemaVersion, 1);
}

TEST_F(F16aSchemaTest, MigrateCreatesTablesAndIndices) {
    F16aSchemaProvider p;
    Status s = p.Migrate(db_, 0, 1);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_TRUE(TableExists("db_connections"));
    EXPECT_TRUE(TableExists("import_tasks"));
    EXPECT_TRUE(IndexExists("idx_db_connections_tenant"));
    EXPECT_TRUE(IndexExists("idx_db_connections_active"));
    EXPECT_TRUE(IndexExists("idx_import_tasks_ns_status"));
    EXPECT_TRUE(IndexExists("idx_import_tasks_running"));
}

TEST_F(F16aSchemaTest, MigrateIsIdempotent) {
    F16aSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
    // already-current (1 -> 1) is a defensive no-op.
    EXPECT_TRUE(p.Migrate(db_, 1, 1).ok());
    // a fresh 0 -> 1 re-run is a no-op via IF NOT EXISTS.
    EXPECT_TRUE(p.Migrate(db_, 0, 1).ok());
}

TEST_F(F16aSchemaTest, UnsupportedStepIsVersionMismatch) {
    F16aSchemaProvider p;
    Status s = p.Migrate(db_, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("VERSION_MISMATCH"), std::string::npos);
}

TEST_F(F16aSchemaTest, ForeignKeysAndColumnsUsable) {
    F16aSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());

    char* err = nullptr;
    // valid insert against existing tenant / namespace.
    std::string ins_conn =
        "INSERT INTO db_connections(ref_id,name,tenant_id,secret_key_id,host,db_name,"
        "registered_by,expires_at) VALUES "
        "('db_conn_a','crm','t1','sk_1','h','d','u1',9999999999999);";
    EXPECT_EQ(sqlite3_exec(db_, ins_conn.c_str(), nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "");
    sqlite3_free(err);
    err = nullptr;

    std::string ins_task =
        "INSERT INTO import_tasks(task_id,ns_id,tenant_id,connection_ref_id,request_json,"
        "text_strategy,submitted_by) VALUES "
        "('import_x','customer_kb','t1','db_conn_a','{}','per_row','u1');";
    EXPECT_EQ(sqlite3_exec(db_, ins_task.c_str(), nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "");
    sqlite3_free(err);
    err = nullptr;

    // FK violation: unknown namespace must be rejected with foreign_keys=ON.
    std::string bad =
        "INSERT INTO import_tasks(task_id,ns_id,tenant_id,connection_ref_id,request_json,"
        "text_strategy,submitted_by) VALUES "
        "('import_y','no_such_ns','t1','db_conn_a','{}','per_row','u1');";
    EXPECT_NE(sqlite3_exec(db_, bad.c_str(), nullptr, nullptr, &err), SQLITE_OK);
    sqlite3_free(err);
}

}  // namespace
}  // namespace cortrix::import
