#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <utility>
#include <vector>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::catalog {
namespace {

// Records each Migrate() call and (optionally) fails, so tests can assert order,
// idempotency, and rollback.
class FakeProvider : public ISchemaProvider {
public:
    FakeProvider(std::string name, int version, std::vector<std::string>* log, bool fail = false)
        : name_(std::move(name)), version_(version), log_(log), fail_(fail) {}

    std::string FeatureName() const override { return name_; }
    int CurrentVersion() const override { return version_; }

    Status Migrate(sqlite3* db, int from, int to) override {
        log_->push_back(name_ + ":" + std::to_string(from) + "->" + std::to_string(to));
        if (fail_) return Status::Internal("provider " + name_ + " boom");
        const std::string ddl = "CREATE TABLE IF NOT EXISTS t_" + name_ + " (id INTEGER)";
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

class SchemaMigratorTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
};

TEST_F(SchemaMigratorTest, MigratesProvidersInRegistrationOrder) {
    std::vector<std::string> log;
    FakeProvider a("alpha", 1, &log);
    FakeProvider b("beta", 2, &log);
    SchemaMigrator m;
    m.Register(&a);
    m.Register(&b);

    Status st = m.MigrateCatalog(db_);
    ASSERT_TRUE(st.ok()) << st.message();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], "alpha:0->1");
    EXPECT_EQ(log[1], "beta:0->2");
    EXPECT_EQ(m.CurrentVersion(db_, "alpha"), 1);
    EXPECT_EQ(m.CurrentVersion(db_, "beta"), 2);
}

TEST_F(SchemaMigratorTest, IdempotentSecondRunSkips) {
    std::vector<std::string> log;
    FakeProvider a("alpha", 1, &log);
    SchemaMigrator m;
    m.Register(&a);

    EXPECT_TRUE(m.MigrateCatalog(db_).ok());
    EXPECT_TRUE(m.MigrateCatalog(db_).ok());  // already at v1 → no second Migrate
    EXPECT_EQ(log.size(), 1u);
    EXPECT_EQ(m.CurrentVersion(db_, "alpha"), 1);
}

TEST_F(SchemaMigratorTest, FailureRollsBackWholeBatch) {
    std::vector<std::string> log;
    FakeProvider a("alpha", 1, &log);
    FakeProvider bad("beta", 1, &log, /*fail=*/true);
    SchemaMigrator m;
    m.Register(&a);
    m.Register(&bad);

    Status st = m.MigrateCatalog(db_);
    EXPECT_FALSE(st.ok());
    // alpha migrated before beta failed, but the whole transaction rolled back,
    // so alpha's version write is reverted too.
    EXPECT_EQ(m.CurrentVersion(db_, "alpha"), 0);
}

TEST_F(SchemaMigratorTest, UnknownFeatureVersionIsZero) {
    SchemaMigrator m;
    // No schema_version table yet → defaults to 0 without crashing.
    EXPECT_EQ(m.CurrentVersion(db_, "never_registered"), 0);
}

}  // namespace
}  // namespace cortrix::catalog
