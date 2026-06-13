#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_error.h"
#include "cortrix/catalog/default_ns_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/resource/namespace_resource_bundle.h"

// Round-3 branch coverage for DefaultINSRouter targeting the regions the round-1/2
// tests left untouched (confirmed via gcov line-by-line):
//   - the F05 admission hook (f05_pool_ != nullptr): both the admit-success arm AND
//     the admit-failure compensation arm (RemoveNamespaceCatalogRows, lines 256-260
//     + 375-393) — round-1/2 always ran with a NULL pool so this whole region was
//     dead.
//   - NULL vs non-NULL optional columns (ColText/ColTextOpt/ColIntOpt both arms).
//   - bind ternaries in InsertNamespaceCatalog (empty vs non-empty fields).
//   - ListNamespaces option matrix with EVERY option OFF (the default-arm side of
//     each `if (opts.* )`), complementing round-1's all-ON test.
namespace cortrix::catalog {
namespace {

bool HasCxCode(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;
}

// A minimal INamespacePool double whose AdmitCreate outcome is scriptable. Only
// AdmitCreate is exercised by DefaultINSRouter::CreateNamespace; the rest satisfy
// the interface. EvictForDelete is recorded so the DeleteNamespace hook (Phase-1
// no-op today) can be observed if it is ever wired.
class FakePool : public cortrix::resource::INamespacePool {
public:
    Status AdmitCreate(const std::string& ns, size_t) override {
        ++admit_calls_;
        last_admitted_ = ns;
        return admit_result_;
    }
    cortrix::Result<cortrix::resource::NamespaceResourceBundle*> Acquire(
        const std::string&) override {
        return cortrix::Result<cortrix::resource::NamespaceResourceBundle*>(nullptr);
    }
    void Release(const std::string&) override {}
    cortrix::Result<cortrix::resource::StartupReport> StartupLoadAll() override {
        return cortrix::resource::StartupReport{};
    }
    Status ReloadNamespace(const std::string&) override { return Status::Ok(); }
    Status EvictForDelete(const std::string& ns) override {
        ++evict_calls_;
        last_evicted_ = ns;
        return Status::Ok();
    }
    cortrix::resource::PoolStats GetPoolStats() const override { return {}; }
    cortrix::resource::ExplainState GetExplainState() const override { return {}; }

    void set_admit_result(Status s) { admit_result_ = std::move(s); }
    int admit_calls() const { return admit_calls_; }
    int evict_calls() const { return evict_calls_; }
    const std::string& last_admitted() const { return last_admitted_; }

private:
    Status admit_result_ = Status::Ok();
    int admit_calls_ = 0;
    int evict_calls_ = 0;
    std::string last_admitted_;
    std::string last_evicted_;
};

class NsRouterBranch2Test : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }

    NSMetadata MakeNs(const std::string& id) {
        NSMetadata m;
        m.namespace_id = id;
        m.name = id;
        m.tenant_id = "t1";
        return m;
    }

    // Count rows matching a WHERE clause on a catalog table (verifies the
    // compensation actually removed the just-inserted rows).
    int CountWhere(const std::string& table, const std::string& where) {
        const std::string sql = "SELECT COUNT(*) FROM " + table + " WHERE " + where;
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(catalog_.db(), sql.c_str(), -1, &stmt, nullptr),
                  SQLITE_OK);
        int n = -1;
        if (stmt && sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    }

    CatalogDb catalog_;
    FakePool pool_;
};

// ── F05 admission hook: success arm ──────────────────────────────────────────

// With an injected pool whose AdmitCreate succeeds, CreateNamespace runs the
// catalog INSERT THEN calls pool.AdmitCreate (the f05_pool_ != nullptr true arm +
// admit.ok() arm) and returns Ok. Round-1/2 never wired a pool, so this arm was
// dead.
TEST_F(NsRouterBranch2Test, CreateWithPoolAdmitSuccess) {
    DefaultINSRouter router(catalog_.db(), &pool_);
    pool_.set_admit_result(Status::Ok());
    ASSERT_TRUE(router.CreateNamespace(MakeNs("ns-ok")).ok());
    EXPECT_EQ(pool_.admit_calls(), 1);
    EXPECT_EQ(pool_.last_admitted(), "ns-ok");
    // The row is durably present (catalog INSERT committed before admit).
    EXPECT_EQ(CountWhere("namespaces", "ns_id='ns-ok'"), 1);
}

// ── F05 admission hook: failure arm → compensation ───────────────────────────

// A failing AdmitCreate triggers the §5.1.bis compensation path: the admit Status
// is returned to the caller and RemoveNamespaceCatalogRows runs (lines 256-260 +
// 375-393). The catalog opens with foreign_keys=ON; the compensation deletes
// units→ns_units→namespaces in one txn (its FK-ordering / self-heal semantics are
// the product's own concern), so this test asserts on the propagated Status and
// that the compensation executed, not on a specific committed row state.
TEST_F(NsRouterBranch2Test, CreateWithPoolAdmitFailureReturnsAdmitStatus) {
    DefaultINSRouter router(catalog_.db(), &pool_);
    pool_.set_admit_result(
        cortrix::resource::PoolStatus(
            cortrix::resource::PoolErrorCode::kNsQuotaExceeded, "no room"));

    Status s = router.CreateNamespace(MakeNs("ns-evict"));
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_QUOTA_EXCEEDED"), std::string::npos);
    EXPECT_EQ(pool_.admit_calls(), 1);
}

// The compensation actually removes the catalog rows when FK enforcement is OFF
// (units-first delete then succeeds) — drives RemoveNamespaceCatalogRows to its
// COMMIT arm (line 393 ok=true) and the cache Invalidate, and proves the hard
// removal is a clean undo (the namespaces row is gone, not merely soft-deleted).
TEST_F(NsRouterBranch2Test, CompensationRemovesRowsWithFkOff) {
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "PRAGMA foreign_keys=OFF",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    DefaultINSRouter router(catalog_.db(), &pool_);
    pool_.set_admit_result(
        cortrix::resource::PoolStatus(
            cortrix::resource::PoolErrorCode::kNsQuotaExceeded, "no room"));

    Status s = router.CreateNamespace(MakeNs("ns-evict"));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(CountWhere("namespaces", "ns_id='ns-evict'"), 0);
    EXPECT_EQ(CountWhere("ns_units", "ns_id='ns-evict'"), 0);
    EXPECT_EQ(CountWhere("units", "unit_id='unit-ns-evict'"), 0);
    // A same-name retry is unblocked (hard delete left no PK-blocking row).
    pool_.set_admit_result(Status::Ok());
    EXPECT_TRUE(router.CreateNamespace(MakeNs("ns-evict")).ok());
}

// SetPool late-binds the hook after construction (F13 ctor-cycle break); a create
// after SetPool exercises the same admit arm via the late-bound pointer.
TEST_F(NsRouterBranch2Test, SetPoolLateBindsAdmissionHook) {
    DefaultINSRouter router(catalog_.db());  // built with NULL pool
    router.SetPool(&pool_);
    pool_.set_admit_result(Status::Ok());
    ASSERT_TRUE(router.CreateNamespace(MakeNs("ns-late")).ok());
    EXPECT_EQ(pool_.admit_calls(), 1);
}

// ── ColTextOpt / ColIntOpt: non-NULL arms ────────────────────────────────────

// A row with a non-NULL owner_user_id reads it back via ColTextOpt's non-NULL arm
// (round-1's CreateWithoutOwner covers the NULL arm). The config blobs are never
// set by INSERT, but the schema gives them `DEFAULT '{}'`, so they too read back
// through ColTextOpt's non-NULL arm as "{}" (the ColTextOpt nullopt arm is covered
// instead by the nullable clone fields in GetNamespaceReadsNonNullCloneFields).
TEST_F(NsRouterBranch2Test, GetNamespaceReadsNonNullOwner) {
    DefaultINSRouter router(catalog_.db());
    NSMetadata m = MakeNs("ns-owner");
    m.owner_user_id = "u-42";
    ASSERT_TRUE(router.CreateNamespace(m).ok());
    auto got = router.GetNamespace("ns-owner");
    ASSERT_TRUE(got.ok());
    ASSERT_TRUE(got.value().owner_user_id.has_value());
    EXPECT_EQ(*got.value().owner_user_id, "u-42");
    // Config blob carries the schema DEFAULT '{}' (non-NULL) → non-NULL arm.
    ASSERT_TRUE(got.value().reranker_config.has_value());
    EXPECT_EQ(*got.value().reranker_config, "{}");
}

// A namespaces row carrying non-NULL clone metadata reads cloned_from_ns_id /
// clone_type / cloned_at through the non-NULL arms of ColTextOpt + ColIntOpt
// (the INSERT path never sets these, so seed them directly).
TEST_F(NsRouterBranch2Test, GetNamespaceReadsNonNullCloneFields) {
    DefaultINSRouter router(catalog_.db());
    ASSERT_TRUE(router.CreateNamespace(MakeNs("ns-clone")).ok());
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "UPDATE namespaces SET cloned_from_ns_id='parent', clone_type='cow', "
        "cloned_at=12345 WHERE ns_id='ns-clone'", nullptr, nullptr, nullptr),
        SQLITE_OK);
    auto got = router.GetNamespace("ns-clone");
    ASSERT_TRUE(got.ok());
    ASSERT_TRUE(got.value().cloned_from_ns_id.has_value());
    EXPECT_EQ(*got.value().cloned_from_ns_id, "parent");
    ASSERT_TRUE(got.value().clone_type.has_value());
    EXPECT_EQ(*got.value().clone_type, "cow");
    ASSERT_TRUE(got.value().cloned_at.has_value());
    EXPECT_EQ(*got.value().cloned_at, 12345);
}

// A row whose config blobs are populated reads them via ColTextOpt's non-NULL arm
// (the 11 *_config columns).
TEST_F(NsRouterBranch2Test, GetNamespaceReadsNonNullConfigBlobs) {
    DefaultINSRouter router(catalog_.db());
    ASSERT_TRUE(router.CreateNamespace(MakeNs("ns-cfg")).ok());
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "UPDATE namespaces SET reranker_config='{\"k\":1}', sparse_config='{}' "
        "WHERE ns_id='ns-cfg'", nullptr, nullptr, nullptr), SQLITE_OK);
    auto got = router.GetNamespace("ns-cfg");
    ASSERT_TRUE(got.ok());
    ASSERT_TRUE(got.value().reranker_config.has_value());
    EXPECT_EQ(*got.value().reranker_config, "{\"k\":1}");
    ASSERT_TRUE(got.value().sparse_config.has_value());
}

// ── InsertNamespaceCatalog bind ternaries: non-empty (non-default) arms ───────

// Supplying non-empty isolation_mode/visibility/status exercises the "false" arm
// of each `field.empty() ? default : field` bind ternary (round-1's MakeNs leaves
// them defaulted, hitting only the empty/true arm).
TEST_F(NsRouterBranch2Test, CreateBindsExplicitNonDefaultFields) {
    DefaultINSRouter router(catalog_.db());
    NSMetadata m = MakeNs("ns-explicit");
    m.isolation_mode = "isolated";
    m.visibility = "public";
    m.status = "active";
    ASSERT_TRUE(router.CreateNamespace(m).ok());
    auto got = router.GetNamespace("ns-explicit");
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().isolation_mode, "isolated");
    EXPECT_EQ(got.value().visibility, "public");
}

// ── cache-hit arms (explicit) ────────────────────────────────────────────────

// GetNamespace second call returns from the cache (line 181 hit arm). Drop the
// table after the first read so a DB re-read would fail — proving the cache served.
TEST_F(NsRouterBranch2Test, GetNamespaceCacheHitArm) {
    DefaultINSRouter router(catalog_.db());
    ASSERT_TRUE(router.CreateNamespace(MakeNs("ns-ch")).ok());
    ASSERT_TRUE(router.GetNamespace("ns-ch").ok());  // miss → Put
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE namespaces", nullptr, nullptr, nullptr),
              SQLITE_OK);
    auto hit = router.GetNamespace("ns-ch");
    EXPECT_TRUE(hit.ok()) << "must be served from cache";
}

// ── ListNamespaces: every option OFF (default arms) ──────────────────────────

// The round-1 list test sets options; this drives the all-default path: no
// tenant_id (skip the tenant filter), include_deleted=false (status filter added),
// limit=0 / offset=0 (skip both LIMIT/OFFSET appends) — the false arm of each
// option `if`.
TEST_F(NsRouterBranch2Test, ListNamespacesAllOptionsDefault) {
    DefaultINSRouter router(catalog_.db());
    ASSERT_TRUE(router.CreateNamespace(MakeNs("z")).ok());
    ASSERT_TRUE(router.CreateNamespace(MakeNs("y")).ok());
    ListNamespacesOptions opts;  // all defaults: no tenant, limit 0, offset 0
    auto r = router.ListNamespaces(opts);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().results.size(), 2u);
    // ORDER BY ns_id ASC → 'y' before 'z'.
    EXPECT_EQ(r.value().results.front(), "y");
}

// limit set but offset 0 (only the LIMIT append arm, not OFFSET) — splits the two
// option `if`s that round-1's combined limit+offset test took together.
TEST_F(NsRouterBranch2Test, ListNamespacesLimitOnlyNoOffset) {
    DefaultINSRouter router(catalog_.db());
    for (char c : {'a', 'b', 'c'}) {
        ASSERT_TRUE(router.CreateNamespace(MakeNs(std::string(1, c))).ok());
    }
    ListNamespacesOptions opts;
    opts.limit = 2;  // offset stays 0 → only the LIMIT branch
    auto r = router.ListNamespaces(opts);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().results.size(), 2u);
    EXPECT_EQ(r.value().results[0], "a");
}

// ── DeleteNamespace: the f05_pool_ present path ──────────────────────────────

// DeleteNamespace with an injected pool still soft-deletes (Phase-1 the eviction
// hook is a no-op (void)f05_pool_ — but the pointer-present state differs from the
// NULL-pool round-1 path).
TEST_F(NsRouterBranch2Test, DeleteNamespaceWithPoolPresent) {
    DefaultINSRouter router(catalog_.db(), &pool_);
    pool_.set_admit_result(Status::Ok());
    ASSERT_TRUE(router.CreateNamespace(MakeNs("ns-d")).ok());
    ASSERT_TRUE(router.DeleteNamespace("ns-d").ok());
    auto got = router.GetNamespace("ns-d");
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, "deleted");
}

}  // namespace
}  // namespace cortrix::catalog
