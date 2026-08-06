#include <gtest/gtest.h>

#include <sqlite3.h>

#include <memory>
#include <string>

#include "cortrix/catalog/bloom_filter.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/default_content_ref_store.h"
#include "cortrix/catalog/default_ns_router.h"
#include "cortrix/catalog/default_unit_router.h"

// S6.1 integration (catalog startup sequence + test #10): wire the whole catalog
// module over one catalog.db and exercise the components together — the order a
// real Server startup follows: open+migrate → routers → ref store → BloomFilter
// rebuild → ready. Then a small cross-component dedup smoke.
namespace cortrix::catalog {
namespace {

// A minimal "CatalogModule" assembled the way prescribes.
struct CatalogModule {
    CatalogDb db;
    std::unique_ptr<DefaultINSRouter> ns_router;
    std::unique_ptr<DefaultUnitRouter> unit_router;
    std::unique_ptr<DefaultContentRefStore> ref_store;
    std::unique_ptr<BloomFilter> bloom;

    Status Start(const std::string& path) {
        Status st = db.Open(path);                       // 1. open + migrate (6+6 tables)
        if (!st.ok()) return st;
        ns_router = std::make_unique<DefaultINSRouter>(db.db());     // 2. NS router
        unit_router = std::make_unique<DefaultUnitRouter>(db.db());  // 3. Unit router
        ref_store = std::make_unique<DefaultContentRefStore>(db.db());// 4. ref store
        bloom = std::make_unique<BloomFilter>(1024 * 1024, 0.01);    // 5. BF init
        return bloom->LoadFromCatalog(db.db());          // 6. BF rebuild → ready
    }
};

TEST(CatalogStartupTest, FullSequenceReachesReady) {
    CatalogModule m;
    ASSERT_TRUE(m.Start(":memory:").ok());
    // All components live + BF ready (empty catalog → ready with 0 entries).
    EXPECT_NE(m.ns_router, nullptr);
    EXPECT_NE(m.unit_router, nullptr);
    EXPECT_NE(m.ref_store, nullptr);
    ASSERT_NE(m.bloom, nullptr);
    EXPECT_TRUE(m.bloom->IsReady());
    EXPECT_FLOAT_EQ(m.bloom->GetLoadProgress(), 1.0f);
}

TEST(CatalogStartupTest, CreateNamespaceThenRouteToUnit) {
    CatalogModule m;
    ASSERT_TRUE(m.Start(":memory:").ok());
    ASSERT_EQ(sqlite3_exec(m.db.db(),
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
        nullptr, nullptr, nullptr), SQLITE_OK);

    NSMetadata meta;
    meta.namespace_id = "docs"; meta.name = "docs"; meta.tenant_id = "t1";
    ASSERT_TRUE(m.ns_router->CreateNamespace(meta).ok());

    // INSRouter → Unit, then UnitRouter reads that Unit (cross-component).
    auto active = m.ns_router->GetActiveUnit("docs");
    ASSERT_TRUE(active.ok()) << active.status().message();
    auto unit = m.unit_router->GetUnit(active.value().unit_id);
    ASSERT_TRUE(unit.ok());
    EXPECT_EQ(unit.value().state, UnitState::kActive);
}

TEST(CatalogStartupTest, DedupPathBloomThenRefStore) {
    // The write/dedup interplay across BloomFilter + ContentRefStore +
    // file_locations: an unseen file misses the BF; after recording it, a rebuilt
    // BF reports it present and the ref store tracks the count.
    CatalogModule m;
    ASSERT_TRUE(m.Start(":memory:").ok());
    ASSERT_EQ(sqlite3_exec(m.db.db(),
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t1',0);"
        "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
        nullptr, nullptr, nullptr), SQLITE_OK);

    // Fresh BF: an unknown hash is definitely absent (dedup miss → would write).
    EXPECT_FALSE(m.bloom->MightContain("fh-1"));

    // Record the file: file_locations row + a content ref + BF.Add (write path).
    ASSERT_EQ(sqlite3_exec(m.db.db(),
        "INSERT INTO file_locations(file_hash, unit_id, ns_id, tenant_id, first_seen_at, created_at) "
        "VALUES('fh-1','u1','ns1','t1',0,0)", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_TRUE(m.ref_store->IncrementRef("fh-1", "ns1", "doc1").ok());
    ASSERT_TRUE(m.bloom->Add("fh-1").ok());

    // Now the BF says "maybe" and the catalog confirms ref_count == 1 (dedup hit).
    EXPECT_TRUE(m.bloom->MightContain("fh-1"));
    auto rc = m.ref_store->GetRefCount("fh-1");
    ASSERT_TRUE(rc.ok());
    EXPECT_EQ(rc.value(), 1);

    // A restart rebuilds the BF from file_locations and still finds fh-1.
    BloomFilter rebuilt(1024 * 1024, 0.01);
    ASSERT_TRUE(rebuilt.LoadFromCatalog(m.db.db()).ok());
    EXPECT_TRUE(rebuilt.IsReady());
    EXPECT_TRUE(rebuilt.MightContain("fh-1"));
    EXPECT_EQ(rebuilt.EstimatedCount(), 1u);
}

TEST(CatalogStartupTest, RestartReopensExistingCatalog) {
    // Persisted catalog.db: a second module Start() on the same file re-migrates
    // idempotently and sees prior data (NS survives).
    std::string path = std::string(::testing::TempDir()) + "catalog_startup_restart.db";
    std::remove(path.c_str());
    {
        CatalogModule m1;
        ASSERT_TRUE(m1.Start(path).ok());
        ASSERT_EQ(sqlite3_exec(m1.db.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        NSMetadata meta;
        meta.namespace_id = "keep"; meta.name = "keep"; meta.tenant_id = "t1";
        ASSERT_TRUE(m1.ns_router->CreateNamespace(meta).ok());
    }
    {
        CatalogModule m2;
        ASSERT_TRUE(m2.Start(path).ok());
        auto got = m2.ns_router->GetNamespace("keep");
        EXPECT_TRUE(got.ok()) << "namespace did not survive restart";
    }
    std::remove(path.c_str());
}

}  // namespace
}  // namespace cortrix::catalog
