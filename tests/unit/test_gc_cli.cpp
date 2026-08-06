// ARCH — gc_cli MaybeRunGcCli unit tests (previously ZERO coverage).
// ParseConfigPath / StatusJson / ReportJson live in an anonymous namespace and
// are not directly reachable; the public entry point is MaybeRunGcCli, which we
// drive end to end against a real temp data dir (catalog.db auto-created by
// CatalogDb::Open) for the happy paths, and a poisoned data dir for the
// catalog-open-failure path. All deterministic: no network, no LLM.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "cortrix/server/gc_cli.h"

namespace cortrix::server {
namespace {

// argv built from std::strings (MaybeRunGcCli takes char* argv[]).
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

class GcCliTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = "/tmp/cortrix_gc_cli_" + std::to_string(getpid());
        data_dir_ = base_ + "/data";
        std::system(("rm -rf " + base_ + " && mkdir -p " + data_dir_).c_str());
    }
    void TearDown() override { std::system(("rm -rf " + base_).c_str()); }

    // Write a minimal config YAML pointing namespace.data_dir at `dir`.
    std::string WriteConfig(const std::string& dir) {
        const std::string path = base_ + "/config.yaml";
        std::ofstream out(path);
        out << "namespace:\n  data_dir: " << dir << "\n";
        out.close();
        return path;
    }

    std::string base_;
    std::string data_dir_;
};

// --- MaybeRunGcCli: argc < 2 → nullopt (caller continues to normal startup) ---
TEST_F(GcCliTest, NoSubcommandReturnsNullopt) {
    Argv a({"cortrix-server"});
    EXPECT_FALSE(MaybeRunGcCli(a.argc(), a.argv()).has_value());
}

// --- MaybeRunGcCli: a non-gc subcommand → nullopt ---
TEST_F(GcCliTest, UnrecognizedSubcommandReturnsNullopt) {
    Argv a({"cortrix-server", "serve"});
    EXPECT_FALSE(MaybeRunGcCli(a.argc(), a.argv()).has_value());
}

TEST_F(GcCliTest, AnotherNonGcSubcommandReturnsNullopt) {
    Argv a({"cortrix-server", "--help"});
    EXPECT_FALSE(MaybeRunGcCli(a.argc(), a.argv()).has_value());
}

// --- gc-status on a fresh, writable data dir → exit 0 (catalog auto-created) ---
TEST_F(GcCliTest, GcStatusSucceedsOnFreshDataDir) {
    const std::string cfg = WriteConfig(data_dir_);
    Argv a({"cortrix-server", "gc-status", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 0);
}

// --- gc-run on a fresh data dir → exit 0 (empty sweep report) ---
TEST_F(GcCliTest, GcRunSucceedsOnFreshDataDir) {
    const std::string cfg = WriteConfig(data_dir_);
    Argv a({"cortrix-server", "gc-run", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 0);
}

// --- restore with no doc_id args → exit 2 (usage error) ---
TEST_F(GcCliTest, RestoreWithoutDocIdsReturnsTwo) {
    const std::string cfg = WriteConfig(data_dir_);
    // Only the --config flag + value after "restore"; no doc_ids.
    Argv a({"cortrix-server", "restore", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 2);
}

// --- catalog-open-failure → exit 1 (data_dir points at a regular file, so
//     "<file>/catalog.db" cannot be opened) ---
TEST_F(GcCliTest, CatalogOpenFailureReturnsOne) {
    // Create a regular file and use it as the data_dir so that appending
    // "/catalog.db" yields an un-openable path.
    const std::string file_path = base_ + "/not_a_dir";
    { std::ofstream(file_path) << "x"; }
    const std::string cfg = WriteConfig(file_path);
    Argv a({"cortrix-server", "gc-status", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 1);
}

// --- a recognized gc subcommand is detected even with a dangling --config (the
//     missing value is tolerated by ParseConfigPath → empty path → default dir) ---
TEST_F(GcCliTest, GcStatusWithDanglingConfigFlagStillRuns) {
    const std::string cfg = WriteConfig(data_dir_);
    // Use CORTRIX_DATA_DIR so the default-config path still resolves to our temp
    // dir even though --config has no value here.
    setenv("CORTRIX_DATA_DIR", data_dir_.c_str(), 1);
    Argv a({"cortrix-server", "gc-status", "--config"});  // dangling flag
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    unsetenv("CORTRIX_DATA_DIR");
    ASSERT_TRUE(rc.has_value());  // still treated as a gc subcommand (not nullopt)
    EXPECT_EQ(*rc, 0);
}

}  // namespace
}  // namespace cortrix::server
