#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_error.h"
#include "cortrix/catalog/default_ns_router.h"
#include "cortrix/catalog/default_unit_router.h"

// S2.3 coverage: DefaultUnitRouter against a real in-memory catalog.db.
namespace cortrix::catalog {
namespace {

bool HasCxCode(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;
}

class UnitRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        ns_router_ = std::make_unique<DefaultINSRouter>(catalog_.db());
        unit_router_ = std::make_unique<DefaultUnitRouter>(catalog_.db());
        // Create an NS → establishes its sole Unit "unit-ns1".
        NSMetadata m;
        m.namespace_id = "ns1"; m.name = "ns1"; m.tenant_id = "t1";
        ASSERT_TRUE(ns_router_->CreateNamespace(m).ok());
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultINSRouter> ns_router_;
    std::unique_ptr<DefaultUnitRouter> unit_router_;
};

TEST_F(UnitRouterTest, GetUnitReturnsPhase1Defaults) {
    auto u = unit_router_->GetUnit("unit-ns1");
    ASSERT_TRUE(u.ok()) << u.status().message();
    EXPECT_EQ(u.value().unit_id, "unit-ns1");
    EXPECT_EQ(u.value().state, UnitState::kActive);
    EXPECT_EQ(u.value().index_type, IndexType::kHnsw);
    EXPECT_EQ(u.value().owner_node_id, "local");
    EXPECT_EQ(u.value().seal_threshold_vectors, INT64_MAX);
}

TEST_F(UnitRouterTest, GetMissingUnitIsUnitNotFound) {
    auto u = unit_router_->GetUnit("nope");
    EXPECT_FALSE(u.ok());
    EXPECT_TRUE(HasCxCode(u.status(), "CX_ERR_UNIT_NOT_FOUND")) << u.status().message();
}

TEST_F(UnitRouterTest, GetUnitState) {
    auto s = unit_router_->GetUnitState("unit-ns1");
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value(), UnitState::kActive);
}

TEST_F(UnitRouterTest, UpdateUnitStateActiveToActiveOk) {
    EXPECT_TRUE(unit_router_->UpdateUnitState("unit-ns1", UnitState::kActive).ok());
}

TEST_F(UnitRouterTest, UpdateUnitStateToSealedIsNotImplemented) {
    Status st = unit_router_->UpdateUnitState("unit-ns1", UnitState::kSealed);
    EXPECT_FALSE(st.ok());
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_NOT_IMPLEMENTED")) << st.message();
}

TEST_F(UnitRouterTest, UpdateStateMissingUnitIsUnitNotFound) {
    Status st = unit_router_->UpdateUnitState("nope", UnitState::kActive);
    EXPECT_FALSE(st.ok());
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_UNIT_NOT_FOUND")) << st.message();
}

TEST_F(UnitRouterTest, UpdateUnitStatsPersists) {
    UnitStats s;
    s.vector_count = 42;
    s.size_bytes = 4096;
    s.last_write_at = 123456;
    ASSERT_TRUE(unit_router_->UpdateUnitStats("unit-ns1", s).ok());

    auto u = unit_router_->GetUnit("unit-ns1");
    ASSERT_TRUE(u.ok());
    EXPECT_EQ(u.value().vector_count, 42);
    EXPECT_EQ(u.value().size_bytes, 4096);
    EXPECT_EQ(u.value().last_write_at, 123456);
}

TEST_F(UnitRouterTest, UpdateStatsMissingUnitIsUnitNotFound) {
    UnitStats s;
    Status st = unit_router_->UpdateUnitStats("nope", s);
    EXPECT_FALSE(st.ok());
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_UNIT_NOT_FOUND")) << st.message();
}

TEST_F(UnitRouterTest, ShouldSealAlwaysFalsePhase1) {
    // Even after a large stats update, the INT_MAX thresholds never trip.
    UnitStats s;
    s.vector_count = 5'000'000;
    s.size_bytes = 5'000'000'000LL;
    ASSERT_TRUE(unit_router_->UpdateUnitStats("unit-ns1", s).ok());
    auto seal = unit_router_->ShouldSeal("unit-ns1");
    ASSERT_TRUE(seal.ok());
    EXPECT_FALSE(seal.value());
}

TEST_F(UnitRouterTest, ShouldSealMissingUnitIsUnitNotFound) {
    auto seal = unit_router_->ShouldSeal("nope");
    EXPECT_FALSE(seal.ok());
    EXPECT_TRUE(HasCxCode(seal.status(), "CX_ERR_UNIT_NOT_FOUND")) << seal.status().message();
}

// ── branch coverage: prepare failures, NULL columns, threshold tripping ──────

// Dropping the units table makes ReadUnitLocked's prepare fail → kInternalError
// (every GetUnit/GetUnitState/UpdateUnitState/ShouldSeal reaches ReadUnitLocked).
TEST_F(UnitRouterTest, GetUnitPrepareFailureIsInternalError) {
    // FK enforcement is ON (catalog_db) and ns_units holds rows referencing
    // units, so a bare DROP hits SQLITE_CONSTRAINT; disable FKs for the drop.
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE units", nullptr, nullptr, nullptr),
              SQLITE_OK);
    auto u = unit_router_->GetUnit("unit-ns1");
    EXPECT_FALSE(u.ok());
    EXPECT_TRUE(HasCxCode(u.status(), "CX_ERR_INTERNAL_ERROR")) << u.status().message();
}

TEST_F(UnitRouterTest, GetUnitStatePrepareFailureIsInternalError) {
    // FK enforcement is ON (catalog_db) and ns_units holds rows referencing
    // units, so a bare DROP hits SQLITE_CONSTRAINT; disable FKs for the drop.
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE units", nullptr, nullptr, nullptr),
              SQLITE_OK);
    auto s = unit_router_->GetUnitState("unit-ns1");
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(HasCxCode(s.status(), "CX_ERR_INTERNAL_ERROR")) << s.status().message();
}

// sealed_at / archived_at are NULL on a fresh Phase-1 Unit → the ColIntOpt NULL
// branch returns nullopt; vector/byte/idle thresholds keep their INT_MAX defaults.
TEST_F(UnitRouterTest, FreshUnitHasNullOptionalTimestamps) {
    auto u = unit_router_->GetUnit("unit-ns1");
    ASSERT_TRUE(u.ok());
    EXPECT_FALSE(u.value().sealed_at.has_value());
    EXPECT_FALSE(u.value().archived_at.has_value());
}

// When sealed_at / archived_at carry real values, ColIntOpt takes its non-NULL
// branch (the other half of the NULL guard).
TEST_F(UnitRouterTest, UnitWithSealedAndArchivedTimestampsReadsThem) {
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "UPDATE units SET sealed_at=111, archived_at=222 WHERE unit_id='unit-ns1'",
        nullptr, nullptr, nullptr), SQLITE_OK);
    auto u = unit_router_->GetUnit("unit-ns1");
    ASSERT_TRUE(u.ok());
    ASSERT_TRUE(u.value().sealed_at.has_value());
    EXPECT_EQ(*u.value().sealed_at, 111);
    ASSERT_TRUE(u.value().archived_at.has_value());
    EXPECT_EQ(*u.value().archived_at, 222);
}

TEST_F(UnitRouterTest, UpdateUnitStatsPrepareFailureIsInternalError) {
    // FK enforcement is ON (catalog_db) and ns_units holds rows referencing
    // units, so a bare DROP hits SQLITE_CONSTRAINT; disable FKs for the drop.
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE units", nullptr, nullptr, nullptr),
              SQLITE_OK);
    UnitStats s;
    Status st = unit_router_->UpdateUnitStats("unit-ns1", s);
    EXPECT_FALSE(st.ok());
    EXPECT_TRUE(HasCxCode(st, "CX_ERR_INTERNAL_ERROR")) << st.message();
}

// ShouldSeal trips on the by_vectors branch once the threshold is lowered below
// the stored vector_count (the OR's true side).
TEST_F(UnitRouterTest, ShouldSealTripsWhenVectorThresholdLowered) {
    UnitStats s;
    s.vector_count = 100;
    ASSERT_TRUE(unit_router_->UpdateUnitStats("unit-ns1", s).ok());
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "UPDATE units SET seal_threshold_vectors=50 WHERE unit_id='unit-ns1'",
        nullptr, nullptr, nullptr), SQLITE_OK);
    auto seal = unit_router_->ShouldSeal("unit-ns1");
    ASSERT_TRUE(seal.ok());
    EXPECT_TRUE(seal.value());
}

// ShouldSeal trips on the by_bytes branch when vectors are under threshold but
// size_bytes is over (the OR's second operand).
TEST_F(UnitRouterTest, ShouldSealTripsWhenByteThresholdLowered) {
    UnitStats s;
    s.size_bytes = 100;
    ASSERT_TRUE(unit_router_->UpdateUnitStats("unit-ns1", s).ok());
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "UPDATE units SET seal_threshold_bytes=50 WHERE unit_id='unit-ns1'",
        nullptr, nullptr, nullptr), SQLITE_OK);
    auto seal = unit_router_->ShouldSeal("unit-ns1");
    ASSERT_TRUE(seal.ok());
    EXPECT_TRUE(seal.value());
}

}  // namespace
}  // namespace cortrix::catalog
