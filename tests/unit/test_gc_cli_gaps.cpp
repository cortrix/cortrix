// gc_cli.cpp gap coverage. Complements test_gc_cli.cpp (GcCliTest) and
// test_gc_cli_full.cpp (GcCliFull), which already cover the nullopt / happy /
// open-failure / restore arms. This file's suite name is GcCliGaps (globally
// unique).
//
// Targets the operation-FAILURE stderr arms that the existing suites never reach
// (gc-status failed -> exit 1 at gc_cli.cpp:71-72; gc-run/purge failed -> exit 1 at
// gc_cli.cpp:80-82). These need a catalog.db that OPENS cleanly (so the open-error
// arm is bypassed) but whose GC query then fails. We get that deterministically by:
//   1. running gc-status once on a fresh dir so CatalogDb::Open migrates the full
//      Catalog schema AND records the schema_version,
//   2. dropping the `file_locations` table directly via sqlite3.
// On the next MaybeRunGcCli call, Open is version-gated (schema_version is already
// current) so it SKIPS re-creating the table; GetStatus / RunOnce then fail their
// "SELECT ... FROM file_locations" prepare and return CX_ERR_GC_INTERNAL -> exit 1.
//
// Note: GcManager::Restore() collects per-doc failures into its OK Result (never
// returns a non-ok Status), so gc_cli.cpp's restore !ok() arm (lines 99-101) is not
// reachable from this entry point and is intentionally not targeted here.
//
// Deterministic: per-process temp data dir (getpid + atomic counter), no network,
// no LLM.
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/server/gc_cli.h"

namespace cortrix::server {
namespace {

struct Argv {
    explicit Argv(std::vector<std::string> args) : storage(std::move(args)) {
        for (auto& s : storage) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
    }
    int argc() const { return static_cast<int>(storage.size()); }
    char** argv() { return ptrs.data(); }
    std::vector<std::string> storage;
    std::vector<char*> ptrs;
};

class GcCliGaps : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> ctr{0};
        base_ = (std::filesystem::temp_directory_path() /
                 ("cortrix_gc_cli_gaps_" + std::to_string(getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1))))
                    .string();
        data_dir_ = base_ + "/data";
        std::filesystem::create_directories(data_dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    std::string WriteConfig(const std::string& dir) {
        const std::string path = base_ + "/config.yaml";
        std::ofstream out(path);
        out << "namespace:\n  data_dir: " << dir << "\n";
        out.close();
        return path;
    }

    // Run gc-status once so the catalog is fully migrated, then DROP file_locations
    // so subsequent GC reads fail (the schema_version row keeps Open version-gated,
    // so the table is NOT re-created on the next Open).
    void MigrateThenBreakCatalog(const std::string& cfg) {
        Argv warm({"cortrix-server", "gc-status", "--config", cfg});
        auto rc = MaybeRunGcCli(warm.argc(), warm.argv());
        ASSERT_TRUE(rc.has_value());
        ASSERT_EQ(*rc, 0) << "warm-up gc-status should succeed on a fresh catalog";

        const std::string catalog_path = data_dir_ + "/catalog.db";
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(catalog_path.c_str(), &db), SQLITE_OK);
        char* err = nullptr;
        // Disable FK enforcement so the DROP is not blocked by dependents, then drop
        // the table the GC status/run queries read.
        sqlite3_exec(db, "PRAGMA foreign_keys=OFF;", nullptr, nullptr, &err);
        sqlite3_free(err);
        err = nullptr;
        ASSERT_EQ(sqlite3_exec(db, "DROP TABLE IF EXISTS file_locations;", nullptr, nullptr, &err),
                  SQLITE_OK)
            << (err ? err : "drop failed");
        sqlite3_free(err);
        sqlite3_close(db);
    }

    std::string base_;
    std::string data_dir_;
};

// gc-status against a catalog whose file_locations table is gone -> the GetStatus
// SELECT prepare fails -> the stderr+return-1 arm (gc_cli.cpp:71-72).
TEST_F(GcCliGaps, GcStatusFailureReturnsOne) {
    const std::string cfg = WriteConfig(data_dir_);
    MigrateThenBreakCatalog(cfg);
    Argv a({"cortrix-server", "gc-status", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 1);
}

// gc-run against the same broken catalog -> the RunOnce sweep fails -> the stderr+
// return-1 arm shared by gc-run/purge (gc_cli.cpp:80-82).
TEST_F(GcCliGaps, GcRunFailureReturnsOne) {
    const std::string cfg = WriteConfig(data_dir_);
    MigrateThenBreakCatalog(cfg);
    Argv a({"cortrix-server", "gc-run", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 1);
}

// purge against the broken catalog -> same RunLocked failure path (gc_cli.cpp:80-82),
// reached through the purge subcommand instead of gc-run.
TEST_F(GcCliGaps, PurgeFailureReturnsOne) {
    const std::string cfg = WriteConfig(data_dir_);
    MigrateThenBreakCatalog(cfg);
    Argv a({"cortrix-server", "purge", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 1);
}

}  // namespace
}  // namespace cortrix::server
