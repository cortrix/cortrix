// test_catalog_coverage_gaps.cpp — residual core-17 CATALOG error-arm coverage.
//
// Each case pins a specific uncovered sqlite ERROR arm (prepare/step/exec/BEGIN
// failure → ROLLBACK + an error Status) identified from the lcov core17 report and
// NOT already covered by the existing catalog fault tests (test_catalog_txn_depth /
// test_content_ref_store / test_default_ns_router / test_errpath_gc /
// test_gc_manager*). The arming techniques are copied verbatim from those files:
//   * "missing schema" — a bare in-memory db (no CatalogDb migration) so the first
//     prepare hits an absent table (see test_errpath_gc MissingSchemaReturnsGcInternal);
//   * "DROP TABLE" after CatalogDb::Open — a later prepare against the dropped table
//     fails kInternalError (see test_content_ref_store *PrepareFailureIsInternalError);
//   * "OuterTxn" — an outer BEGIN held on the same handle makes an inner BEGIN fail
//     (see test_catalog_txn_depth OuterTxn);
//   * "bad path" — sqlite3_open of an uncreatable path fails (catalog_db open arm).
// No syscall fault-injection, no sleeps, no background threads (gc_thread cv-loop is
// E2E-only and intentionally untouched here).
//
// gtest suite names are GLOBAL — every fixture/suite is module-prefixed "CatCovGap*".

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_error.h"
#include "cortrix/catalog/default_content_ref_store.h"
#include "cortrix/catalog/default_ns_router.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/config/config.h"

namespace cortrix::catalog {
namespace {

bool CatCovGapHasCx(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().find(cx) != std::string::npos;
}

// RAII outer txn (mirrors test_catalog_txn_depth OuterTxn).
struct CatCovGapOuterTxn {
    sqlite3* db;
    explicit CatCovGapOuterTxn(sqlite3* d) : db(d) {
        EXPECT_EQ(sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    ~CatCovGapOuterTxn() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
};

// ===========================================================================
// CatalogDb::OpenAndConfigure open-fail arm (catalog_db.cpp 49-57). A path under a
// directory that does not exist makes sqlite3_open fail → Status::Internal.
// ===========================================================================
TEST(CatCovGapCatalogDbTest, OpenBadPathIsInternalError) {
    CatalogDb db;
    // Parent directory "/qcg_no_such_dir_xyz" does not exist → sqlite3_open fails.
    Status s = db.Open("/qcg_no_such_dir_xyz/sub/dir/catalog.db");
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("catalog.db open failed"), std::string::npos)
        << "message: " << s.message();
}

// ===========================================================================
// DefaultINSRouter::ReadUnitLocked prepare-fail arm (default_ns_router.cpp 125-126).
// Reached via GetActiveUnit after the units table is dropped: ActiveUnitIdLocked
// resolves the mapping (ns_units intact) but ReadUnitLocked's prepare against the
// dropped units table fails kInternalError.
// ===========================================================================
class CatCovGapNsRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        router_ = std::make_unique<DefaultINSRouter>(catalog_.db());
        NSMetadata m; m.namespace_id = "nsA"; m.name = "nsA"; m.tenant_id = "t1";
        ASSERT_TRUE(router_->CreateNamespace(m).ok());
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultINSRouter> router_;
};

TEST_F(CatCovGapNsRouterTest, GetActiveUnitReadUnitPrepareFailIsInternalError) {
    // Drop units only: the ns_units mapping still resolves the unit_id, then the
    // ReadUnitLocked SELECT against the absent units table fails to prepare.
    // units is an FK parent (ns_units references it), so disable FK enforcement on
    // this connection first or the DROP returns SQLITE_CONSTRAINT.
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE units", nullptr, nullptr, nullptr),
              SQLITE_OK);
    Result<UnitDescriptor> u = router_->GetActiveUnit("nsA");
    ASSERT_FALSE(u.ok());
    EXPECT_TRUE(CatCovGapHasCx(u.status(), "CX_ERR")) << u.status().message();
}

// RemoveNamespaceCatalogRows BEGIN-fail arm (default_ns_router.cpp 377-379), reached
// via CreateNamespace's compensation path: an outer txn is held so the compensating
// removal's inner BEGIN fails and returns early (best-effort, no crash). We drive the
// compensation by making the row already exist so a second create's INSERT collides —
// but the simplest deterministic trigger of the BEGIN-fail is a direct call while an
// outer txn is open. RemoveNamespaceCatalogRows is reached internally; here we assert
// the public CreateNamespace stays well-behaved when its own txn is blocked.
TEST_F(CatCovGapNsRouterTest, CreateUnderOuterTxnIsTransactionRetry) {
    CatCovGapOuterTxn outer(catalog_.db());  // inner BEGIN of InsertNamespaceCatalog fails
    NSMetadata m; m.namespace_id = "nsB"; m.name = "nsB"; m.tenant_id = "t1";
    Status s = router_->CreateNamespace(m);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(CatCovGapHasCx(s, "CX_ERR_TRANSACTION_RETRY")) << s.message();
}

// DeleteNamespace not-found step result arm: a delete of an absent NS reports
// NS_NOT_FOUND via the sqlite3_changes()==0 branch (default_ns_router.cpp 416-418).
TEST_F(CatCovGapNsRouterTest, DeleteMissingNsIsNsNotFound) {
    Status s = router_->DeleteNamespace("does-not-exist");
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(CatCovGapHasCx(s, "CX_ERR_NS_NOT_FOUND")) << s.message();
}

// ===========================================================================
// DefaultContentRefStore mid-transaction error arms not covered by
// test_content_ref_store (which only drops the FIRST-touched table). Dropping
// file_locations (NOT content_refs) lets the probe + upsert on content_refs succeed,
// then the ref_count++ UPDATE on the absent file_locations fails to prepare →
// kInternalError + ROLLBACK (default_content_ref_store.cpp 98-102).
// ===========================================================================
class CatCovGapRefStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        store_ = std::make_unique<DefaultContentRefStore>(catalog_.db());
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultContentRefStore> store_;
};

TEST_F(CatCovGapRefStoreTest, IncrementRefCountBumpPrepareFailIsInternalError) {
    // content_refs intact (probe + upsert succeed, newly_added=true) but
    // file_locations gone → the ref_count++ UPDATE prepare fails.
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE file_locations", nullptr, nullptr, nullptr),
              SQLITE_OK);
    Status s = store_->IncrementRef("fhX", "ns1", "doc1");
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(CatCovGapHasCx(s, "CX_ERR_INTERNAL")) << s.message();
}

// IncrementRef COMMIT-fail arm is hard to force deterministically; instead cover the
// BEGIN-fail commit-retry symmetry on the GetRefCount read path with a dropped table.
TEST_F(CatCovGapRefStoreTest, GetRefCountPrepareFailIsInternalError) {
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE file_locations", nullptr, nullptr, nullptr),
              SQLITE_OK);
    Result<int> r = store_->GetRefCount("fhX");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(CatCovGapHasCx(r.status(), "CX_ERR_INTERNAL")) << r.status().message();
}

}  // namespace
}  // namespace cortrix::catalog

// ===========================================================================
// GcManager additional error arms (gc_manager.cpp). RunOnce/Purge propagate a
// Stage 2 select prepare-fail (92-93 + 132-133); Restore's content_refs select
// prepare-fail records a per-doc CX_ERR_GC_INTERNAL failure (296-297); Restore's
// inner BEGIN-fail under an outer txn likewise fails the doc (311-313); EnqueueBlob's
// prepare-fail surfaces CX_ERR_GC_INTERNAL (385-386). All driven with a bare,
// schema-less in-memory db (the existing MissingSchemaReturnsGcInternal technique).
// ===========================================================================
namespace cortrix::catalog::gc {
namespace {

// A schema-less in-memory db: every GcManager prepare hits an absent table.
class CatCovGapGcMissingSchemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        mgr_ = std::make_unique<GcManager>(db_, &sink_, cfg_);
    }
    void TearDown() override { if (db_) sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    NullBlobGcSink sink_;
    GcConfig cfg_;
    std::unique_ptr<GcManager> mgr_;
};

TEST_F(CatCovGapGcMissingSchemaTest, RunOnceStage2SelectPrepareFailIsGcInternal) {
    auto r = mgr_->RunOnce();
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_GC_INTERNAL"), std::string::npos)
        << "message: " << r.status().message();
}

TEST_F(CatCovGapGcMissingSchemaTest, PurgeStage2SelectPrepareFailIsGcInternal) {
    auto r = mgr_->Purge();
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_GC_INTERNAL"), std::string::npos)
        << "message: " << r.status().message();
}

TEST_F(CatCovGapGcMissingSchemaTest, EnqueueBlobPrepareFailIsGcInternal) {
    Status s = mgr_->EnqueueBlob("aa/aabbcc", "aabbcc");
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_GC_INTERNAL"), std::string::npos)
        << "message: " << s.message();
}

TEST_F(CatCovGapGcMissingSchemaTest, RestoreContentRefsSelectPrepareFailIsPerDocInternal) {
    // The content_refs SELECT prepare fails → the doc is recorded as a per-id
    // CX_ERR_GC_INTERNAL failure (Restore returns Ok with a populated `failed`).
    auto r = mgr_->Restore({"doc-1"});
    ASSERT_TRUE(r.ok());  // the method itself does not fail; the doc does
    ASSERT_EQ(r.value().succeeded.size(), 0u);
    ASSERT_EQ(r.value().failed.size(), 1u);
    EXPECT_EQ(r.value().failed[0].first, "doc-1");
    EXPECT_EQ(r.value().failed[0].second, "CX_ERR_GC_INTERNAL");
}

}  // namespace
}  // namespace cortrix::catalog::gc

// ===========================================================================
// GcManager::Restore inner BEGIN-fail arm (gc_manager.cpp 311-313): a fully-migrated
// catalog.db with a soft-deleted content_refs row that resolves a file_hash, but an
// outer txn is held so the per-doc BEGIN fails → that doc is reported failed.
// ===========================================================================
namespace cortrix::catalog::gc {
namespace {

class CatCovGapGcRestoreBeginTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        // Seed FK parents + a soft-deleted content_refs row keyed to doc-1 so the
        // file_hash lookup (the pre-BEGIN read) succeeds and Restore proceeds to BEGIN.
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);"
            "INSERT INTO content_refs (file_hash, ns_id, doc_id, target_unit_id, status, created_at) "
            "VALUES ('fh1','ns1','doc-1','','deleted',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        mgr_ = std::make_unique<GcManager>(catalog_.db(), &sink_, cfg_);
    }
    CatalogDb catalog_;
    NullBlobGcSink sink_;
    GcConfig cfg_;
    std::unique_ptr<GcManager> mgr_;
};

TEST_F(CatCovGapGcRestoreBeginTest, RestoreBeginFailUnderOuterTxnFailsDoc) {
    cortrix::catalog::CatCovGapOuterTxn outer(catalog_.db());  // inner BEGIN fails
    auto r = mgr_->Restore({"doc-1"});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().succeeded.size(), 0u);
    ASSERT_EQ(r.value().failed.size(), 1u);
    EXPECT_EQ(r.value().failed[0].first, "doc-1");
    EXPECT_EQ(r.value().failed[0].second, "CX_ERR_GC_INTERNAL");
}

}  // namespace
}  // namespace cortrix::catalog::gc

// ===========================================================================
// SchemaMigrator Exec BEGIN-fail arm (schema_migrator.cpp 14-18 via RunProviders'
// `Exec(db,"BEGIN")`). An outer txn held on the handle makes the migrator's inner
// BEGIN fail → CX_ERR_SCHEMA_MIGRATION_FAILED. (The schema_version CREATE TABLE runs
// first and succeeds; the BEGIN is the second Exec.)
// ===========================================================================
namespace cortrix::catalog {
namespace {

// Minimal provider (mirrors test_schema_migrator's fake): bumps one feature to v1.
class CatCovGapProvider : public ISchemaProvider {
public:
    std::string FeatureName() const override { return "catcovgap_feat"; }
    int CurrentVersion() const override { return 1; }
    Status Migrate(sqlite3* db, int, int) override {
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS catcovgap_t (id INTEGER)",
                     nullptr, nullptr, nullptr);
        return Status::Ok();
    }
};

TEST(CatCovGapSchemaMigratorTest, MigrateUnderOuterTxnBeginFailIsMigrationFailed) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CatCovGapProvider p;
    SchemaMigrator m;
    m.Register(&p);

    // Hold an outer txn so RunProviders' Exec(db,"BEGIN") fails (cannot nest).
    ASSERT_EQ(sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
    Status s = m.MigrateCatalog(db);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_MIGRATION_FAILED"), std::string::npos)
        << "message: " << s.message();
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::catalog
