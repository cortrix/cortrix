#include <gtest/gtest.h>

#include <sqlite3.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_error.h"
#include "cortrix/catalog/default_content_ref_store.h"
#include "cortrix/catalog/default_ns_router.h"
#include "cortrix/catalog/schema_provider.h"

// DEPTH — transaction-control failure → CX_ERR_TRANSACTION_RETRY (kTransactionRetry)
// in the catalog write paths, plus the untested SchemaMigrator::MigrateUnit public
// entry.
//
// Deterministic fault trigger (no syscall fault-injection, no sleeps): the catalog
// txn writers (DefaultINSRouter::InsertNamespaceCatalog via CreateNamespace, and
// DefaultContentRefStore::IncrementRef / DecrementRef) all begin with an inner
// `sqlite3_exec(db, "BEGIN", ...)`. If an OUTER transaction is already open on the
// SAME borrowed handle, that inner BEGIN fails with SQLITE_ERROR ("cannot start a
// transaction within a transaction") → the code returns kTransactionRetry. This is
// fully deterministic and exercises exactly the BEGIN-fail arm. (The fi::FailOp
// syscall seam is unsuitable here: catalog uses :memory:/short txns where BEGIN
// issues no faultable syscall, and mmap reads bypass ::pread — see global mem
// fault-sweep arm-window note.)
//
// Suite names are globally unique (CatalogTxnDepth / SchemaMigratorUnitDepth).
namespace cortrix::catalog {
namespace {

bool HasCxCode(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;  // starts_with
}

// RAII: hold an outer txn open on `db` for the test body, ROLLBACK on scope exit.
struct OuterTxn {
    sqlite3* db;
    explicit OuterTxn(sqlite3* d) : db(d) {
        EXPECT_EQ(sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    ~OuterTxn() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
};

// ---------------- DefaultINSRouter txn control ----------------

class CatalogTxnDepthNsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        router_ = std::make_unique<DefaultINSRouter>(catalog_.db());
    }
    NSMetadata MakeNs(const std::string& id) {
        NSMetadata m; m.namespace_id = id; m.name = id; m.tenant_id = "t1";
        return m;
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultINSRouter> router_;
};

TEST_F(CatalogTxnDepthNsTest, CreateNamespaceBeginFailIsTransactionRetry) {
    OuterTxn outer(catalog_.db());  // inner BEGIN will fail
    Status s = router_->CreateNamespace(MakeNs("ns1"));
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(HasCxCode(s, "CX_ERR_TRANSACTION_RETRY")) << s.message();
}

TEST_F(CatalogTxnDepthNsTest, TransactionRetryIsRetryableTransient) {
    // The kTransactionRetry identity is retryable/transient with a 50ms hint — the
    // contract a caller reads to back off + retry (not surface as permanent).
    const CatalogErrorInfo& info = GetCatalogErrorInfo(CatalogErrorCode::kTransactionRetry);
    EXPECT_STREQ(info.cx_code, "CX_ERR_TRANSACTION_RETRY");
    EXPECT_TRUE(info.retryable);
}

TEST_F(CatalogTxnDepthNsTest, CreateNamespaceRecoversAfterOuterTxnEnds) {
    {
        OuterTxn outer(catalog_.db());
        Status blocked = router_->CreateNamespace(MakeNs("ns_recover"));
        ASSERT_TRUE(HasCxCode(blocked, "CX_ERR_TRANSACTION_RETRY"));
        // outer rolls back here
    }
    // With the conflicting txn gone, the retry succeeds (the row was never written
    // by the blocked attempt — BEGIN failed before any INSERT).
    Status ok = router_->CreateNamespace(MakeNs("ns_recover"));
    EXPECT_TRUE(ok.ok()) << ok.message();
    EXPECT_TRUE(router_->GetNamespace("ns_recover").ok());
}

// ---------------- DefaultContentRefStore txn control ----------------

class CatalogTxnDepthRefTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        SeedFileLocation("fh1");
        store_ = std::make_unique<DefaultContentRefStore>(catalog_.db());
    }
    void SeedFileLocation(const std::string& file_hash, int ref_count = 0) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO file_locations (file_hash, unit_id, ns_id, tenant_id, "
            "ref_count, first_seen_at, status, deleted_at, created_at) "
            "VALUES (?, 'u1', 'ns1', 't1', ?, 0, 'active', NULL, 0)";
        ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
        sqlite3_bind_text(stmt, 1, file_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, ref_count);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultContentRefStore> store_;
};

TEST_F(CatalogTxnDepthRefTest, IncrementRefBeginFailIsTransactionRetry) {
    OuterTxn outer(catalog_.db());
    Status s = store_->IncrementRef("fh1", "ns1", "doc1");
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(HasCxCode(s, "CX_ERR_TRANSACTION_RETRY")) << s.message();
}

TEST_F(CatalogTxnDepthRefTest, DecrementRefBeginFailIsTransactionRetry) {
    // Seed a positive ref_count first (outside any conflicting txn) so Decrement
    // has work to do, then block its BEGIN.
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());
    OuterTxn outer(catalog_.db());
    auto r = store_->DecrementRef("fh1", "ns1", "doc1");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(HasCxCode(r.status(), "CX_ERR_TRANSACTION_RETRY")) << r.status().message();
}

TEST_F(CatalogTxnDepthRefTest, IncrementRefRecoversAfterOuterTxnEnds) {
    {
        OuterTxn outer(catalog_.db());
        ASSERT_TRUE(HasCxCode(store_->IncrementRef("fh1", "ns1", "doc1"),
                              "CX_ERR_TRANSACTION_RETRY"));
    }
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());
    auto c = store_->GetRefCount("fh1");
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 1);  // the blocked attempt wrote nothing; only the retry counted
}

}  // namespace
}  // namespace cortrix::catalog

// ---------------- SchemaMigrator::MigrateUnit (untested public entry) ----------------
namespace cortrix::catalog {
namespace {

// FakeProvider mirroring test_schema_migrator.cpp: records each Migrate() call,
// creates one table, optionally fails.
class UnitFakeProvider : public ISchemaProvider {
public:
    UnitFakeProvider(std::string name, int version, std::vector<std::string>* log,
                     bool fail = false)
        : name_(std::move(name)), version_(version), log_(log), fail_(fail) {}
    std::string FeatureName() const override { return name_; }
    int CurrentVersion() const override { return version_; }
    Status Migrate(sqlite3* db, int from, int to) override {
        if (log_) log_->push_back(name_ + ":" + std::to_string(from) + "->" + std::to_string(to));
        if (fail_) return Status::Internal("provider " + name_ + " boom");
        const std::string ddl = "CREATE TABLE IF NOT EXISTS u_" + name_ + " (id INTEGER)";
        char* err = nullptr;
        sqlite3_exec(db, ddl.c_str(), nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        return Status::Ok();
    }
private:
    std::string name_;
    int version_;
    std::vector<std::string>* log_;
    bool fail_;
};

bool UnitTableExists(sqlite3* db, const std::string& table) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
        db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1,
        &stmt, nullptr), SQLITE_OK);
    bool found = false;
    if (stmt) {
        sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
        found = (sqlite3_step(stmt) == SQLITE_ROW);
    }
    sqlite3_finalize(stmt);
    return found;
}

class SchemaMigratorUnitDepthTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
};

TEST_F(SchemaMigratorUnitDepthTest, MigrateUnitRunsProvidersInOrder) {
    std::vector<std::string> log;
    UnitFakeProvider a("alpha", 1, &log);
    UnitFakeProvider b("beta", 2, &log);
    SchemaMigrator m;
    m.Register(&a);
    m.Register(&b);

    Status st = m.MigrateUnit(db_, "unit-xyz");
    ASSERT_TRUE(st.ok()) << st.message();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], "alpha:0->1");
    EXPECT_EQ(log[1], "beta:0->2");
    EXPECT_TRUE(UnitTableExists(db_, "u_alpha"));
    EXPECT_TRUE(UnitTableExists(db_, "u_beta"));
    EXPECT_EQ(m.CurrentVersion(db_, "alpha"), 1);
    EXPECT_EQ(m.CurrentVersion(db_, "beta"), 2);
}

TEST_F(SchemaMigratorUnitDepthTest, MigrateUnitIgnoresUnitIdString) {
    // The unit_id parameter is reserved (routing/logging); two different ids over
    // fresh DBs produce identical migration outcomes.
    std::vector<std::string> log_a, log_b;
    UnitFakeProvider pa("feat", 1, &log_a);
    SchemaMigrator ma; ma.Register(&pa);
    ASSERT_TRUE(ma.MigrateUnit(db_, "unit-aaa").ok());

    sqlite3* db2 = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db2), SQLITE_OK);
    UnitFakeProvider pb("feat", 1, &log_b);
    SchemaMigrator mb; mb.Register(&pb);
    ASSERT_TRUE(mb.MigrateUnit(db2, "").ok());  // empty unit_id is accepted too

    EXPECT_EQ(log_a, log_b);
    EXPECT_EQ(ma.CurrentVersion(db_, "feat"), 1);
    EXPECT_EQ(mb.CurrentVersion(db2, "feat"), 1);
    sqlite3_close(db2);
}

TEST_F(SchemaMigratorUnitDepthTest, MigrateUnitIsIdempotentOnSecondRun) {
    std::vector<std::string> log;
    UnitFakeProvider a("alpha", 1, &log);
    SchemaMigrator m;
    m.Register(&a);
    EXPECT_TRUE(m.MigrateUnit(db_, "u1").ok());
    EXPECT_TRUE(m.MigrateUnit(db_, "u1").ok());  // already at v1 → no second Migrate
    EXPECT_EQ(log.size(), 1u);
}

TEST_F(SchemaMigratorUnitDepthTest, MigrateUnitFailureRollsBackWholeBatch) {
    std::vector<std::string> log;
    UnitFakeProvider a("alpha", 1, &log);
    UnitFakeProvider bad("beta", 1, &log, /*fail=*/true);
    SchemaMigrator m;
    m.Register(&a);
    m.Register(&bad);

    Status st = m.MigrateUnit(db_, "u1");
    EXPECT_FALSE(st.ok());
    // alpha ran before beta failed, but the whole txn rolled back → alpha's
    // version write reverted.
    EXPECT_EQ(m.CurrentVersion(db_, "alpha"), 0);
}

}  // namespace
}  // namespace cortrix::catalog
