#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_error.h"
#include "cortrix/catalog/default_ns_router.h"
#include "cortrix/catalog/default_unit_router.h"

// BREADTH op-matrix coverage for the catalog routers. Complements
// test_default_ns_router.cpp / test_unit_router.cpp with additional matrices:
// ListNamespaces (limit x offset x include_deleted x tenant) combinations,
// UpdateUnitStats round-trip over value matrices, UpdateUnitState transition
// matrix, and prepare-failure arms (table dropped) across the router methods.
//
// Suite names are globally unique: NsRouterOpsMatrix* / UnitRouterOpsMatrix*.
namespace cortrix::catalog {
namespace {

bool HasCxCode(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;  // starts_with
}

// Disable FKs and drop a table so the next prepare against it fails.
void DropTableFk(sqlite3* db, const char* table) {
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, (std::string("DROP TABLE ") + table).c_str(),
                           nullptr, nullptr, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
}

// --------------------------- NS router fixture ------------------------------
class NsRouterOpsMatrixBase : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1',0),('t2',0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        router_ = std::make_unique<DefaultINSRouter>(catalog_.db());
    }
    NSMetadata MakeNs(const std::string& id, const std::string& tenant = "t1") {
        NSMetadata m;
        m.namespace_id = id;
        m.name = id;
        m.tenant_id = tenant;
        return m;
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultINSRouter> router_;
};

// -- ListNamespaces: limit x offset matrix over a fixed 6-NS corpus ----------
struct PageCase {
    int limit;
    int offset;
    size_t expect_size;
    std::string expect_first;  // empty -> don't check
};
class NsRouterOpsMatrixPaging : public NsRouterOpsMatrixBase,
                                public ::testing::WithParamInterface<PageCase> {};

TEST_P(NsRouterOpsMatrixPaging, PaginatesAscending) {
    // Corpus ns-a..ns-f (sorted ascending by ns_id).
    for (char c = 'a'; c <= 'f'; ++c)
        ASSERT_TRUE(router_->CreateNamespace(MakeNs(std::string("ns-") + c)).ok());

    const auto p = GetParam();
    ListNamespacesOptions opts;
    opts.limit = p.limit;
    opts.offset = p.offset;
    auto r = router_->ListNamespaces(opts);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().results.size(), p.expect_size)
        << "limit=" << p.limit << " offset=" << p.offset;
    if (!p.expect_first.empty() && !r.value().results.empty())
        EXPECT_EQ(r.value().results.front(), p.expect_first);
    // Meta envelope is always well-formed.
    EXPECT_EQ(r.value().meta.api_version, "v1");
    EXPECT_EQ(r.value().meta.node_id, "local");
}

INSTANTIATE_TEST_SUITE_P(
    Pages, NsRouterOpsMatrixPaging,
    ::testing::Values(
        PageCase{0, 0, 6, "ns-a"},   // no limit
        PageCase{2, 0, 2, "ns-a"},
        PageCase{2, 2, 2, "ns-c"},
        PageCase{2, 4, 2, "ns-e"},
        PageCase{3, 0, 3, "ns-a"},
        PageCase{3, 3, 3, "ns-d"},
        PageCase{5, 0, 5, "ns-a"},
        PageCase{10, 0, 6, "ns-a"},  // limit > corpus
        PageCase{2, 5, 1, "ns-f"},   // tail partial page
        PageCase{2, 6, 0, ""},       // offset past end
        PageCase{1, 0, 1, "ns-a"},
        PageCase{1, 1, 1, "ns-b"},
        PageCase{1, 2, 1, "ns-c"},
        PageCase{1, 3, 1, "ns-d"},
        PageCase{1, 4, 1, "ns-e"},
        PageCase{1, 5, 1, "ns-f"},
        PageCase{1, 6, 0, ""},
        PageCase{3, 4, 2, "ns-e"},   // limit beyond tail
        PageCase{4, 0, 4, "ns-a"},
        PageCase{4, 4, 2, "ns-e"},
        PageCase{6, 0, 6, "ns-a"},
        PageCase{6, 3, 3, "ns-d"},
        PageCase{100, 5, 1, "ns-f"}));

// -- ListNamespaces: include_deleted x tenant filter matrix ------------------
struct FilterCase {
    bool include_deleted;
    std::string tenant;      // empty -> no tenant filter
    size_t expect_size;
};
class NsRouterOpsMatrixFilter : public NsRouterOpsMatrixBase,
                                public ::testing::WithParamInterface<FilterCase> {};

TEST_P(NsRouterOpsMatrixFilter, FiltersByDeletedAndTenant) {
    // t1: ns1, ns2 (ns2 deleted). t2: ns3.
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns1", "t1")).ok());
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns2", "t1")).ok());
    ASSERT_TRUE(router_->CreateNamespace(MakeNs("ns3", "t2")).ok());
    ASSERT_TRUE(router_->DeleteNamespace("ns2").ok());

    const auto p = GetParam();
    ListNamespacesOptions opts;
    opts.include_deleted = p.include_deleted;
    if (!p.tenant.empty()) opts.tenant_id = p.tenant;
    auto r = router_->ListNamespaces(opts);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().results.size(), p.expect_size)
        << "include_deleted=" << p.include_deleted << " tenant=" << p.tenant;
}

INSTANTIATE_TEST_SUITE_P(
    Filters, NsRouterOpsMatrixFilter,
    ::testing::Values(
        FilterCase{false, "", 2},     // ns1, ns3 (ns2 excluded)
        FilterCase{true, "", 3},      // all three
        FilterCase{false, "t1", 1},   // ns1 only (ns2 deleted)
        FilterCase{true, "t1", 2},    // ns1, ns2
        FilterCase{false, "t2", 1},   // ns3
        FilterCase{true, "t2", 1},    // ns3
        FilterCase{false, "no-such", 0},
        FilterCase{true, "no-such", 0}));

// -- Create/Get round-trip over an isolation x visibility matrix -------------
struct NsModeCase { std::string isolation; std::string visibility; };
class NsRouterOpsMatrixModes : public NsRouterOpsMatrixBase,
                               public ::testing::WithParamInterface<NsModeCase> {};

TEST_P(NsRouterOpsMatrixModes, ModesRoundTrip) {
    const auto p = GetParam();
    NSMetadata m = MakeNs("ns-mode");
    m.isolation_mode = p.isolation;
    m.visibility = p.visibility;
    ASSERT_TRUE(router_->CreateNamespace(m).ok());
    auto got = router_->GetNamespace("ns-mode");
    ASSERT_TRUE(got.ok()) << got.status().message();
    EXPECT_EQ(got.value().isolation_mode, p.isolation);
    EXPECT_EQ(got.value().visibility, p.visibility);
    EXPECT_EQ(got.value().status, "active");
}

INSTANTIATE_TEST_SUITE_P(
    Modes, NsRouterOpsMatrixModes,
    ::testing::Values(NsModeCase{"shared", "tenant"},
                      NsModeCase{"shared", "private"},
                      NsModeCase{"shared", "public"},
                      NsModeCase{"isolated", "tenant"},
                      NsModeCase{"isolated", "private"},
                      NsModeCase{"isolated", "public"}));

// -- NS prepare-failure matrix: each method fails kInternalError on dropped tbl
struct NsPrepareCase { std::string drop_table; std::string method; };
class NsRouterOpsMatrixPrepareFail
    : public NsRouterOpsMatrixBase,
      public ::testing::WithParamInterface<NsPrepareCase> {};

TEST_P(NsRouterOpsMatrixPrepareFail, MethodReportsInternalError) {
    const auto p = GetParam();
    DropTableFk(catalog_.db(), p.drop_table.c_str());
    Status st;
    if (p.method == "get") {
        st = router_->GetNamespace("x").status();
    } else if (p.method == "list") {
        st = router_->ListNamespaces({}).status();
    } else if (p.method == "delete") {
        st = router_->DeleteNamespace("x");
    } else if (p.method == "create") {
        st = router_->CreateNamespace(MakeNs("x"));
    } else if (p.method == "active_unit") {
        st = router_->GetActiveUnit("x").status();
    }
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_INTERNAL_ERROR"))
        << "method=" << p.method << " drop=" << p.drop_table
        << " msg=" << st.message();
}

INSTANTIATE_TEST_SUITE_P(
    Methods, NsRouterOpsMatrixPrepareFail,
    ::testing::Values(NsPrepareCase{"namespaces", "get"},
                      NsPrepareCase{"namespaces", "list"},
                      NsPrepareCase{"namespaces", "delete"},
                      NsPrepareCase{"namespaces", "create"},
                      NsPrepareCase{"ns_units", "active_unit"}));

// -- NS not-found matrix: missing id across the lookup methods ----------------
class NsRouterOpsMatrixNotFound
    : public NsRouterOpsMatrixBase,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(NsRouterOpsMatrixNotFound, MissingNsIsNsNotFound) {
    const std::string method = GetParam();
    Status st;
    if (method == "get") st = router_->GetNamespace("ghost").status();
    else if (method == "lookup") st = router_->LookupUnits("ghost").status();
    else if (method == "active") st = router_->GetActiveUnit("ghost").status();
    else if (method == "delete") st = router_->DeleteNamespace("ghost");
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_NS_NOT_FOUND"))
        << "method=" << method << " msg=" << st.message();
}

INSTANTIATE_TEST_SUITE_P(Methods, NsRouterOpsMatrixNotFound,
                         ::testing::Values(std::string("get"),
                                           std::string("lookup"),
                                           std::string("active"),
                                           std::string("delete")));

// -- Create -> GetActiveUnit -> LookupUnits 1:1 mapping over an id matrix -----
class NsRouterOpsMatrixMapping
    : public NsRouterOpsMatrixBase,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(NsRouterOpsMatrixMapping, EstablishesSingleUnitNamedAfterNs) {
    const std::string ns = GetParam();
    ASSERT_TRUE(router_->CreateNamespace(MakeNs(ns)).ok());
    auto active = router_->GetActiveUnit(ns);
    ASSERT_TRUE(active.ok()) << active.status().message();
    EXPECT_EQ(active.value().unit_id, "unit-" + ns);
    EXPECT_EQ(active.value().state, UnitState::kActive);
    EXPECT_EQ(active.value().index_type, IndexType::kHnsw);
    auto units = router_->LookupUnits(ns);
    ASSERT_TRUE(units.ok());
    ASSERT_EQ(units.value().size(), 1u);  // Phase 1 = 1:1
    EXPECT_EQ(units.value()[0].unit_id, "unit-" + ns);
}

INSTANTIATE_TEST_SUITE_P(Ids, NsRouterOpsMatrixMapping,
                         ::testing::Values(std::string("ns1"),
                                           std::string("ns-with-dash"),
                                           std::string("ns_underscore"),
                                           std::string("NS-UPPER"),
                                           std::string("ns.dot"),
                                           std::string("a"),
                                           std::string("ns123"),
                                           std::string("longnamespaceid12345")));

// -- Delete -> soft-delete status flip + double-delete over an id matrix ------
class NsRouterOpsMatrixDelete
    : public NsRouterOpsMatrixBase,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(NsRouterOpsMatrixDelete, SoftDeleteFlipsStatusAndSecondDeleteIsNotFound) {
    const std::string ns = GetParam();
    ASSERT_TRUE(router_->CreateNamespace(MakeNs(ns)).ok());
    ASSERT_TRUE(router_->DeleteNamespace(ns).ok());
    auto got = router_->GetNamespace(ns);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, "deleted");
    Status second = router_->DeleteNamespace(ns);
    EXPECT_TRUE(HasCxCode(second, "CX_ERR_NS_NOT_FOUND")) << second.message();
}

INSTANTIATE_TEST_SUITE_P(Ids, NsRouterOpsMatrixDelete,
                         ::testing::Values(std::string("d1"), std::string("d2"),
                                           std::string("d-3"), std::string("d_4"),
                                           std::string("D5"), std::string("d6")));

// --------------------------- Unit router fixture ----------------------------
class UnitRouterOpsMatrixBase : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1',0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        ns_router_ = std::make_unique<DefaultINSRouter>(catalog_.db());
        unit_router_ = std::make_unique<DefaultUnitRouter>(catalog_.db());
        NSMetadata m;
        m.namespace_id = "ns1"; m.name = "ns1"; m.tenant_id = "t1";
        ASSERT_TRUE(ns_router_->CreateNamespace(m).ok());
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultINSRouter> ns_router_;
    std::unique_ptr<DefaultUnitRouter> unit_router_;
};

// -- UpdateUnitStats round-trip over a (vectors, bytes, last_write) matrix ----
struct StatsCase { int64_t vectors; int64_t bytes; int64_t last_write; };
class UnitRouterOpsMatrixStats
    : public UnitRouterOpsMatrixBase,
      public ::testing::WithParamInterface<StatsCase> {};

TEST_P(UnitRouterOpsMatrixStats, StatsRoundTrip) {
    const auto p = GetParam();
    UnitStats s;
    s.vector_count = p.vectors;
    s.size_bytes = p.bytes;
    s.last_write_at = p.last_write;
    ASSERT_TRUE(unit_router_->UpdateUnitStats("unit-ns1", s).ok());
    auto u = unit_router_->GetUnit("unit-ns1");
    ASSERT_TRUE(u.ok()) << u.status().message();
    EXPECT_EQ(u.value().vector_count, p.vectors);
    EXPECT_EQ(u.value().size_bytes, p.bytes);
    EXPECT_EQ(u.value().last_write_at, p.last_write);
    // State stays active; the descriptor still reports Phase-1 defaults.
    EXPECT_EQ(u.value().state, UnitState::kActive);
    EXPECT_EQ(u.value().seal_threshold_vectors, INT64_MAX);
}

INSTANTIATE_TEST_SUITE_P(
    Stats, UnitRouterOpsMatrixStats,
    ::testing::Values(StatsCase{0, 0, 0}, StatsCase{1, 1, 1},
                      StatsCase{42, 4096, 123456},
                      StatsCase{1000, 1'000'000, 1'700'000'000},
                      StatsCase{5'000'000, 5'000'000'000LL, 9'999'999'999LL},
                      StatsCase{INT64_MAX, INT64_MAX, INT64_MAX}));

// -- UpdateUnitState transition matrix (Phase-1: only active->active is OK) ----
struct StateCase { UnitState target; bool expect_ok; };
class UnitRouterOpsMatrixState
    : public UnitRouterOpsMatrixBase,
      public ::testing::WithParamInterface<StateCase> {};

TEST_P(UnitRouterOpsMatrixState, TransitionGate) {
    const auto p = GetParam();
    Status st = unit_router_->UpdateUnitState("unit-ns1", p.target);
    EXPECT_EQ(st.ok(), p.expect_ok);
    if (!p.expect_ok)
        EXPECT_TRUE(HasCxCode(st, "CX_ERR_NOT_IMPLEMENTED")) << st.message();
    // State remains active either way (Phase-1 only persists active->active).
    auto s = unit_router_->GetUnitState("unit-ns1");
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value(), UnitState::kActive);
}

INSTANTIATE_TEST_SUITE_P(
    Transitions, UnitRouterOpsMatrixState,
    ::testing::Values(StateCase{UnitState::kActive, true},
                      StateCase{UnitState::kSealing, false},
                      StateCase{UnitState::kSealed, false},
                      StateCase{UnitState::kArchived, false},
                      StateCase{UnitState::kMigrating, false}));

// -- ShouldSeal threshold-trip matrix (lower a threshold below stored stats) --
struct SealCase {
    int64_t vectors;
    int64_t bytes;
    const char* threshold_col;
    int64_t threshold_val;
    bool expect_seal;
};
class UnitRouterOpsMatrixSeal
    : public UnitRouterOpsMatrixBase,
      public ::testing::WithParamInterface<SealCase> {};

TEST_P(UnitRouterOpsMatrixSeal, ThresholdTrips) {
    const auto p = GetParam();
    UnitStats s;
    s.vector_count = p.vectors;
    s.size_bytes = p.bytes;
    ASSERT_TRUE(unit_router_->UpdateUnitStats("unit-ns1", s).ok());
    std::string sql = std::string("UPDATE units SET ") + p.threshold_col + "=" +
                      std::to_string(p.threshold_val) + " WHERE unit_id='unit-ns1'";
    ASSERT_EQ(sqlite3_exec(catalog_.db(), sql.c_str(), nullptr, nullptr, nullptr),
              SQLITE_OK) << sqlite3_errmsg(catalog_.db());
    auto seal = unit_router_->ShouldSeal("unit-ns1");
    ASSERT_TRUE(seal.ok()) << seal.status().message();
    EXPECT_EQ(seal.value(), p.expect_seal);
}

INSTANTIATE_TEST_SUITE_P(
    Seals, UnitRouterOpsMatrixSeal,
    ::testing::Values(
        // ShouldSeal uses >= : stat >= threshold trips.
        SealCase{100, 0, "seal_threshold_vectors", 50, true},    // over
        SealCase{100, 0, "seal_threshold_vectors", 100, true},   // exactly at (>=)
        SealCase{100, 0, "seal_threshold_vectors", 101, false},  // under
        SealCase{100, 0, "seal_threshold_vectors", 200, false},  // well under
        SealCase{0, 100, "seal_threshold_bytes", 50, true},      // over
        SealCase{0, 100, "seal_threshold_bytes", 100, true},     // exactly at (>=)
        SealCase{0, 100, "seal_threshold_bytes", 101, false},
        SealCase{0, 100, "seal_threshold_bytes", 200, false}));

// -- Unit not-found matrix across the unit methods ----------------------------
class UnitRouterOpsMatrixNotFound
    : public UnitRouterOpsMatrixBase,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(UnitRouterOpsMatrixNotFound, MissingUnitIsUnitNotFound) {
    const std::string method = GetParam();
    Status st;
    if (method == "get") st = unit_router_->GetUnit("nope").status();
    else if (method == "state") st = unit_router_->GetUnitState("nope").status();
    else if (method == "update_state")
        st = unit_router_->UpdateUnitState("nope", UnitState::kActive);
    else if (method == "update_stats")
        st = unit_router_->UpdateUnitStats("nope", UnitStats{});
    else if (method == "should_seal") st = unit_router_->ShouldSeal("nope").status();
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_UNIT_NOT_FOUND"))
        << "method=" << method << " msg=" << st.message();
}

INSTANTIATE_TEST_SUITE_P(Methods, UnitRouterOpsMatrixNotFound,
                         ::testing::Values(std::string("get"),
                                           std::string("state"),
                                           std::string("update_state"),
                                           std::string("update_stats"),
                                           std::string("should_seal")));

// -- Unit prepare-failure matrix: units table dropped -> kInternalError -------
class UnitRouterOpsMatrixPrepareFail
    : public UnitRouterOpsMatrixBase,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(UnitRouterOpsMatrixPrepareFail, MethodReportsInternalError) {
    const std::string method = GetParam();
    DropTableFk(catalog_.db(), "units");
    Status st;
    if (method == "get") st = unit_router_->GetUnit("unit-ns1").status();
    else if (method == "state") st = unit_router_->GetUnitState("unit-ns1").status();
    else if (method == "update_stats")
        st = unit_router_->UpdateUnitStats("unit-ns1", UnitStats{});
    else if (method == "should_seal")
        st = unit_router_->ShouldSeal("unit-ns1").status();
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_INTERNAL_ERROR"))
        << "method=" << method << " msg=" << st.message();
}

INSTANTIATE_TEST_SUITE_P(Methods, UnitRouterOpsMatrixPrepareFail,
                         ::testing::Values(std::string("get"),
                                           std::string("state"),
                                           std::string("update_stats"),
                                           std::string("should_seal")));

}  // namespace
}  // namespace cortrix::catalog
