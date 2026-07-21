#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_error.h"
#include "cortrix/catalog/default_ns_router.h"

// S2.2 coverage: DefaultINSRouter against a real in-memory catalog.db. Exercises
// the 6 methods end-to-end + the §8.1 error codes on the not-found / duplicate
// paths.
namespace cortrix::catalog {
namespace {

// Does this error Status carry the given CX_ERR_ code? CatalogStatus prefixes the
// message with the token, so a prefix check identifies the catalog error.
bool HasCxCode(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;  // starts_with
}

class DefaultNSRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        // A tenant for the NS rows to reference (namespaces.tenant_id FK).
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        router_ = std::make_unique<DefaultINSRouter>(catalog_.db());
    }

    NSMetadata MakeNs(const std::string& id) {
        NSMetadata m;
        m.namespace_id = id;
        m.name = id;
        m.tenant_id = "t1";
        return m;
    }

    CatalogDb catalog_;
    std::unique_ptr<DefaultINSRouter> router_;
};

TEST_F(DefaultNSRouterTest, CreateThenGetNamespace) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns1")).ok());

    auto got = router_->GetNamespace("ns1");
    ASSERT_TRUE(got.ok()) << got.status().message();
    EXPECT_EQ(got.value().namespace_id, "ns1");
    EXPECT_EQ(got.value().tenant_id, "t1");
    // Phase-1 defaults applied by the schema.
    EXPECT_EQ(got.value().isolation_mode, "shared");
    EXPECT_EQ(got.value().visibility, "tenant");
    EXPECT_EQ(got.value().status, "active");
}

TEST_F(DefaultNSRouterTest, GetMissingNamespaceIsNsNotFound) {
    auto got = router_->GetNamespace("ghost");
    EXPECT_FALSE(got.ok());
    EXPECT_TRUE(HasCxCode(got.status(), "CX_ERR_NS_NOT_FOUND"))
        << got.status().message();
    EXPECT_EQ(got.status().code(), StatusCode::kNotFound);
}

TEST_F(DefaultNSRouterTest, DuplicateCreateIsAlreadyExists) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("dup")).ok());
    Status second = router_->CreateNamespace(MakeNs("dup"));
    EXPECT_FALSE(second.ok());
    EXPECT_TRUE(HasCxCode(second, "CX_ERR_NS_ALREADY_EXISTS")) << second.message();
    EXPECT_EQ(second.code(), StatusCode::kAlreadyExists);
}

TEST_F(DefaultNSRouterTest, CreateEstablishesSingleUnitMapping) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns1")).ok());

    auto active = router_->GetActiveUnit("ns1");
    ASSERT_TRUE(active.ok()) << active.status().message();
    EXPECT_EQ(active.value().unit_id, "unit-ns1");
    EXPECT_EQ(active.value().tenant_id, "t1");
    // Phase-1 Unit defaults.
    EXPECT_EQ(active.value().state, UnitState::kActive);
    EXPECT_EQ(active.value().index_type, IndexType::kHnsw);
    EXPECT_EQ(active.value().owner_node_id, "local");

    auto units = router_->LookupUnits("ns1");
    ASSERT_TRUE(units.ok());
    ASSERT_EQ(units.value().size(), 1u);          // Phase 1 = 1:1
    EXPECT_EQ(units.value()[0].unit_id, "unit-ns1");
}

TEST_F(DefaultNSRouterTest, LookupUnitsMissingNsIsNsNotFound) {
    auto units = router_->LookupUnits("ghost");
    EXPECT_FALSE(units.ok());
    EXPECT_TRUE(HasCxCode(units.status(), "CX_ERR_NS_NOT_FOUND")) << units.status().message();
}

TEST_F(DefaultNSRouterTest, ListNamespacesFiltersAndExcludesDeleted) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns-a")).ok());
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns-b")).ok());
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns-c")).ok());

    auto all = router_->ListNamespaces();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all.value().results.size(), 3u);
    EXPECT_EQ(all.value().meta.api_version, "v1");
    EXPECT_EQ(all.value().meta.node_id, "local");
    EXPECT_FLOAT_EQ(all.value().meta.coverage_ratio, 1.0f);
    // sorted ascending.
    EXPECT_EQ(all.value().results.front(), "ns-a");

    // delete one → excluded by default, included with include_deleted.
    ASSERT_TRUE(router_->DeleteNamespace("ns-b").ok());
    auto active = router_->ListNamespaces();
    ASSERT_TRUE(active.ok());
    EXPECT_EQ(active.value().results.size(), 2u);

    ListNamespacesOptions with_deleted;
    with_deleted.include_deleted = true;
    auto incl = router_->ListNamespaces(with_deleted);
    ASSERT_TRUE(incl.ok());
    EXPECT_EQ(incl.value().results.size(), 3u);
}

TEST_F(DefaultNSRouterTest, ListNamespacesTenantFilter) {
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t2', 0)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns-t1")).ok());
    NSMetadata other = MakeNs("ns-t2");
    other.tenant_id = "t2";
    ASSERT_TRUE(router_->CreateNamespace(other).ok());

    ListNamespacesOptions only_t2;
    only_t2.tenant_id = "t2";
    auto r = router_->ListNamespaces(only_t2);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().results.size(), 1u);
    EXPECT_EQ(r.value().results[0], "ns-t2");
}

TEST_F(DefaultNSRouterTest, DeleteThenGetReflectsSoftDelete) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns1")).ok());
    ASSERT_TRUE(router_->DeleteNamespace("ns1").ok());
    // soft-deleted row still readable, status flips to 'deleted' (cache was
    // invalidated on delete so this reflects the DB, not a stale 'active').
    auto got = router_->GetNamespace("ns1");
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, "deleted");
}

TEST_F(DefaultNSRouterTest, DeleteMissingIsNsNotFound) {
    Status st = router_->DeleteNamespace("ghost");
    EXPECT_FALSE(st.ok());
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_NS_NOT_FOUND")) << st.message();
}

TEST_F(DefaultNSRouterTest, DoubleDeleteSecondIsNsNotFound) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns1")).ok());
    ASSERT_TRUE(router_->DeleteNamespace("ns1").ok());
    Status second = router_->DeleteNamespace("ns1");  // already deleted
    EXPECT_FALSE(second.ok());
    EXPECT_TRUE(HasCxCode(second, "CX_ERR_NS_NOT_FOUND")) << second.message();
}

TEST_F(DefaultNSRouterTest, CacheServesRepeatGet) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns1")).ok());
    auto a = router_->GetNamespace("ns1");
    auto b = router_->GetNamespace("ns1");  // cache hit path
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().namespace_id, b.value().namespace_id);
}

TEST_F(DefaultNSRouterTest, EmptyNsIdRejected) {
    Status st = router_->CreateNamespace(MakeNs(""));
    EXPECT_FALSE(st.ok());
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_INVALID_CONFIG_JSON")) << st.message();
}

// ── branch coverage: error / prepare-failure / edge paths ────────────────────

// Drop a catalog table so the next prepare against it fails (prepare-error branch).
static void DropTable(sqlite3* db, const char* table) {
    // FK enforcement is ON and child tables may hold referencing rows; a bare
    // DROP of a parent table would hit SQLITE_CONSTRAINT.
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, (std::string("DROP TABLE ") + table).c_str(),
                           nullptr, nullptr, nullptr), SQLITE_OK);
}

// Non-default fields survive the INSERT and read back via the ColTextOpt non-NULL
// branch (owner_user_id), plus isolation/visibility overrides.
TEST_F(DefaultNSRouterTest, CreatePreservesNonDefaultFields) {
    NSMetadata m = MakeNs("ns-cfg");
    m.isolation_mode = "isolated";
    m.visibility = "private";
    m.owner_user_id = "user-7";
    ASSERT_TRUE(router_->CreateNamespace(m).ok());
    auto got = router_->GetNamespace("ns-cfg");
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().isolation_mode, "isolated");
    EXPECT_EQ(got.value().visibility, "private");
    ASSERT_TRUE(got.value().owner_user_id.has_value());
    EXPECT_EQ(*got.value().owner_user_id, "user-7");
}

// owner_user_id unset → the INSERT binds NULL and ColTextOpt returns nullopt.
TEST_F(DefaultNSRouterTest, CreateWithoutOwnerReadsNullOwner) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns-no-owner")).ok());
    auto got = router_->GetNamespace("ns-no-owner");
    ASSERT_TRUE(got.ok());
    EXPECT_FALSE(got.value().owner_user_id.has_value());
}

// FK violation (unknown tenant) makes the namespaces INSERT step fail — not a
// UNIQUE/PK violation → the kInternalError branch (not kNsAlreadyExists).
TEST_F(DefaultNSRouterTest, CreateWithUnknownTenantIsInternalError) {
    NSMetadata m = MakeNs("ns-bad");
    m.tenant_id = "no-such-tenant";
    Status s = router_->CreateNamespace(m);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(HasCxCode(s, "CX_ERR_INTERNAL_ERROR")) << s.message();
}

// Dropping namespaces makes the CreateNamespace INSERT prepare fail → kInternalError.
TEST_F(DefaultNSRouterTest, CreatePrepareFailureIsInternalError) {
    DropTable(catalog_.db(), "namespaces");
    Status s = router_->CreateNamespace(MakeNs("ns-x"));
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(HasCxCode(s, "CX_ERR_INTERNAL_ERROR")) << s.message();
}

// Dropping units makes the per-Unit INSERT prepare fail after the ns row inserted
// → rollback → kInternalError.
TEST_F(DefaultNSRouterTest, CreateUnitInsertPrepareFailureRollsBack) {
    DropTable(catalog_.db(), "units");
    Status s = router_->CreateNamespace(MakeNs("ns-u"));
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(HasCxCode(s, "CX_ERR_INTERNAL_ERROR")) << s.message();
}

TEST_F(DefaultNSRouterTest, GetNamespacePrepareFailureIsInternalError) {
    DropTable(catalog_.db(), "namespaces");
    auto r = router_->GetNamespace("anything");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(HasCxCode(r.status(), "CX_ERR_INTERNAL_ERROR")) << r.status().message();
}

TEST_F(DefaultNSRouterTest, ListNamespacesPrepareFailureIsInternalError) {
    DropTable(catalog_.db(), "namespaces");
    auto r = router_->ListNamespaces({});
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(HasCxCode(r.status(), "CX_ERR_INTERNAL_ERROR")) << r.status().message();
}

TEST_F(DefaultNSRouterTest, DeletePrepareFailureIsInternalError) {
    DropTable(catalog_.db(), "namespaces");
    Status s = router_->DeleteNamespace("anything");
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(HasCxCode(s, "CX_ERR_INTERNAL_ERROR")) << s.message();
}

// ActiveUnitIdLocked prepare failure (ns_units dropped) → kInternalError.
TEST_F(DefaultNSRouterTest, GetActiveUnitPrepareFailureIsInternalError) {
    DropTable(catalog_.db(), "ns_units");
    auto u = router_->GetActiveUnit("anything");
    EXPECT_FALSE(u.ok());
    EXPECT_TRUE(HasCxCode(u.status(), "CX_ERR_INTERNAL_ERROR")) << u.status().message();
}

// GetActiveUnit on an unknown NS → ActiveUnitIdLocked finds no mapping row →
// kNsNotFound.
TEST_F(DefaultNSRouterTest, GetActiveUnitMissingNsIsNsNotFound) {
    auto u = router_->GetActiveUnit("ghost");
    EXPECT_FALSE(u.ok());
    EXPECT_TRUE(HasCxCode(u.status(), "CX_ERR_NS_NOT_FOUND")) << u.status().message();
}

// A ns_units mapping whose referenced units row is gone → ReadUnitLocked finds no
// row → kUnitNotFound (FK turned off to stage the torn state).
TEST_F(DefaultNSRouterTest, GetActiveUnitDanglingMappingIsUnitNotFound) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns-dangle")).ok());
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "PRAGMA foreign_keys=OFF;",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "DELETE FROM units WHERE unit_id='unit-ns-dangle'",
        nullptr, nullptr, nullptr), SQLITE_OK);
    auto u = router_->GetActiveUnit("ns-dangle");
    EXPECT_FALSE(u.ok());
    EXPECT_TRUE(HasCxCode(u.status(), "CX_ERR_UNIT_NOT_FOUND")) << u.status().message();
}

// GetActiveUnit second call serves the Unit descriptor from the unit cache: drop
// the units table after the first read and confirm the cached value is returned.
TEST_F(DefaultNSRouterTest, GetActiveUnitServesUnitFromCache) {
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns-uc")).ok());
    ASSERT_TRUE(router_->GetActiveUnit("ns-uc").ok());  // populates the unit cache
    DropTable(catalog_.db(), "units");  // mapping survives; ReadUnit would now fail
    auto u = router_->GetActiveUnit("ns-uc");
    EXPECT_TRUE(u.ok()) << "Unit descriptor must come from the TTL cache";
    EXPECT_EQ(u.value().unit_id, "unit-ns-uc");
}

// ListNamespaces honors limit + offset (the two opts-string-building branches).
TEST_F(DefaultNSRouterTest, ListNamespacesLimitAndOffset) {
    for (char c : {'a', 'b', 'c', 'd'}) {
        ASSERT_TRUE(router_->CreateNamespace(MakeNs(std::string(1, c))).ok());
    }
    ListNamespacesOptions opts;
    opts.limit = 2;
    opts.offset = 1;  // skip 'a' → ['b','c']
    auto r = router_->ListNamespaces(opts);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().results.size(), 2u);
    EXPECT_EQ(r.value().results[0], "b");
    EXPECT_EQ(r.value().results[1], "c");
}

}  // namespace
}  // namespace cortrix::catalog
