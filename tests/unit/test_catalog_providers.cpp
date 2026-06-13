#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <utility>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_schema.h"
#include "cortrix/catalog/schema_provider.h"

// S1.2 + S1.3 coverage: catalog provider registration order (F12 first) and
// multi-provider startup migration through CatalogDb::Open(path, providers).
namespace cortrix::catalog {
namespace {

// A downstream-Feature stand-in. Records the order in which its Migrate() runs
// (shared log), creates one table, and optionally:
//  - references F12's units table via FK (proves F12 must migrate first), or
//  - fails (proves the whole batch rolls back atomically).
class FakeFeatureProvider : public ISchemaProvider {
public:
    FakeFeatureProvider(std::string name, std::vector<std::string>* order_log,
                        bool fk_to_units = false, bool fail = false)
        : name_(std::move(name)), order_log_(order_log),
          fk_to_units_(fk_to_units), fail_(fail) {}

    std::string FeatureName() const override { return name_; }
    int CurrentVersion() const override { return 1; }

    Status Migrate(sqlite3* db, int /*from*/, int /*to*/) override {
        if (order_log_) order_log_->push_back(name_);
        if (fail_) return Status::Internal(name_ + " intentional failure");

        // Table name derived from the feature id; optionally FK→units(unit_id)
        // so creation only succeeds once F12 has already created `units`.
        std::string ddl = "CREATE TABLE feat_" + name_ + " (id INTEGER PRIMARY KEY";
        if (fk_to_units_) {
            ddl += ", unit_id TEXT REFERENCES units(unit_id)";
        }
        ddl += ")";
        char* err = nullptr;
        if (sqlite3_exec(db, ddl.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "sqlite error";
            sqlite3_free(err);
            return Status::Internal(name_ + " ddl failed: " + msg);
        }
        return Status::Ok();
    }

private:
    std::string name_;
    std::vector<std::string>* order_log_;
    bool fk_to_units_;
    bool fail_;
};

bool TableExists(sqlite3* db, const std::string& table) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
        db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1,
        &stmt, nullptr), SQLITE_OK) << sqlite3_errmsg(db);
    bool found = false;
    if (stmt) {
        sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
        found = (sqlite3_step(stmt) == SQLITE_ROW);
    }
    sqlite3_finalize(stmt);
    return found;
}

// S1.3: Open(path, providers) registers F12 first, then the extra providers in
// the given order; all tables end up present and F12 ran before them.
TEST(CatalogProvidersTest, MultiProviderStartupMigration) {
    std::vector<std::string> order;
    // Two downstream stand-ins, the first FK-referencing units (needs F12 first).
    FakeFeatureProvider f09("F09", &order, /*fk_to_units=*/true);
    FakeFeatureProvider f03("F03", &order);

    CatalogDb catalog;
    Status st = catalog.Open(":memory:", {&f09, &f03});
    ASSERT_TRUE(st.ok()) << st.message();

    sqlite3* db = catalog.db();
    // F12 base tables + both downstream tables exist.
    EXPECT_TRUE(TableExists(db, "units"));        // F12 base
    EXPECT_TRUE(TableExists(db, "namespaces"));   // F12 base
    EXPECT_TRUE(TableExists(db, "feat_F09"));     // downstream (FK→units)
    EXPECT_TRUE(TableExists(db, "feat_F03"));     // downstream

    // Registration/execution order: extras run in given order (F12 is not in the
    // log — it is the C++ F12SchemaProvider, not a FakeFeatureProvider).
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "F09");
    EXPECT_EQ(order[1], "F03");

    // schema_version records each migrated feature at v1.
    EXPECT_EQ(SchemaMigrator().CurrentVersion(db, "F12"), kF12SchemaVersion);
    EXPECT_EQ(SchemaMigrator().CurrentVersion(db, "F09"), 1);
    EXPECT_EQ(SchemaMigrator().CurrentVersion(db, "F03"), 1);
}

// S1.2: F12 must migrate before a downstream provider whose table FK-references
// units. If ordering were wrong this DDL would fail; success proves F12-first.
TEST(CatalogProvidersTest, F12RunsBeforeDownstreamFkProvider) {
    std::vector<std::string> order;
    FakeFeatureProvider needs_units("F25", &order, /*fk_to_units=*/true);

    CatalogDb catalog;
    Status st = catalog.Open(":memory:", {&needs_units});
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(TableExists(catalog.db(), "feat_F25"));
}

// S1.3 atomicity: a failing downstream provider rolls back the WHOLE batch,
// including F12's base tables — the migrator runs all providers in one tx.
TEST(CatalogProvidersTest, DownstreamFailureRollsBackEntireBatch) {
    std::vector<std::string> order;
    FakeFeatureProvider ok("F09", &order);
    FakeFeatureProvider boom("F03", &order, /*fk_to_units=*/false, /*fail=*/true);

    CatalogDb catalog;
    Status st = catalog.Open(":memory:", {&ok, &boom});
    // Open() fails (migration aborted) and closes the handle.
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(catalog.db(), nullptr);
}

// On an on-disk db, a failed batch must leave the file with NO catalog tables
// (true rollback, not partial). Re-open cleanly afterwards to confirm usability.
TEST(CatalogProvidersTest, FailedBatchLeavesNoPartialSchemaOnDisk) {
    std::string path = std::string(::testing::TempDir()) + "catalog_atomic.db";
    std::remove(path.c_str());

    std::vector<std::string> order;
    FakeFeatureProvider boom("F03", &order, /*fk_to_units=*/false, /*fail=*/true);
    {
        CatalogDb c1;
        Status st = c1.Open(path, {&boom});
        ASSERT_FALSE(st.ok());
    }
    // Inspect the file directly: F12's units table must not be present.
    sqlite3* probe = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &probe), SQLITE_OK);
    EXPECT_FALSE(TableExists(probe, "units"))
        << "rollback left F12 base tables behind — batch was not atomic";
    sqlite3_close(probe);

    // A subsequent clean Open (no failing provider) succeeds and builds F12.
    {
        CatalogDb c2;
        Status st = c2.Open(path);
        ASSERT_TRUE(st.ok()) << st.message();
        EXPECT_TRUE(TableExists(c2.db(), "units"));
    }
    std::remove(path.c_str());
}

// The S1.1 single-arg Open is unchanged: F12-only, no extra providers.
TEST(CatalogProvidersTest, SingleArgOpenIsF12Only) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    EXPECT_TRUE(TableExists(catalog.db(), "units"));
    EXPECT_EQ(SchemaMigrator().CurrentVersion(catalog.db(), "F12"), kF12SchemaVersion);
}

// nullptr providers in the list are ignored (defensive: matches
// SchemaMigrator::Register skipping nullptr).
TEST(CatalogProvidersTest, NullProvidersIgnored) {
    std::vector<std::string> order;
    FakeFeatureProvider f09("F09", &order);
    CatalogDb catalog;
    Status st = catalog.Open(":memory:", {nullptr, &f09, nullptr});
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(TableExists(catalog.db(), "feat_F09"));
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "F09");
}

}  // namespace
}  // namespace cortrix::catalog
