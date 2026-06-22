// gc_cli.cpp MaybeRunGcCli — additional arg-parsing / exit-code coverage that
// complements test_gc_cli.cpp (which owns the GcCliTest suite). This file's suite
// name is GcCliFull (globally unique). It pins the purge subcommand, the restore
// happy path (exit 0 on a fresh catalog — nothing to restore is still success),
// the restore-with-doc-ids parse path, and the missing-doc_id (exit 2) /
// open-failure (exit 1) arms from the alternate subcommands.
//
// Deterministic: drives MaybeRunGcCli end-to-end against a per-process temp data
// dir (unique via getpid + an atomic counter so parallel -j2 cases never collide).
// CatalogDb::Open auto-creates catalog.db; no network, no LLM.
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
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

class GcCliFull : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> ctr{0};
        base_ = (std::filesystem::temp_directory_path() /
                 ("cortrix_gc_cli_full_" + std::to_string(getpid()) + "_" +
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

    std::string base_;
    std::string data_dir_;
};

// --- purge on a fresh data dir → exit 0 (empty hard-delete report) -------------
TEST_F(GcCliFull, PurgeSucceedsOnFreshDataDir) {
    const std::string cfg = WriteConfig(data_dir_);
    Argv a({"cortrix-server", "purge", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 0);
}

// --- restore with explicit doc_ids → exit 0 (parses doc_ids, no-op on fresh db) -
TEST_F(GcCliFull, RestoreWithDocIdsSucceeds) {
    const std::string cfg = WriteConfig(data_dir_);
    // doc_ids both before AND after the --config flag to exercise the skip-flag
    // branch in the arg loop (`if (a == "--config") { ++i; continue; }`).
    Argv a({"cortrix-server", "restore", "doc-1", "--config", cfg, "doc-2"});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 0);
}

// --- restore with ONLY a positional doc_id (no --config) → exit 0 --------------
TEST_F(GcCliFull, RestoreSinglePositionalDocId) {
    // No --config: ParseConfigPath returns "" → LoadConfig default. Point the
    // default data dir at our temp dir so the catalog opens cleanly.
    setenv("CORTRIX_DATA_DIR", data_dir_.c_str(), 1);
    Argv a({"cortrix-server", "restore", "only-doc"});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    unsetenv("CORTRIX_DATA_DIR");
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 0);
}

// --- restore with no doc_ids → exit 2 (usage error) ---------------------------
TEST_F(GcCliFull, RestoreNoDocIdsIsExitTwo) {
    const std::string cfg = WriteConfig(data_dir_);
    Argv a({"cortrix-server", "restore", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 2);
}

// --- purge against an un-openable catalog (data_dir is a regular file) → exit 1 -
TEST_F(GcCliFull, PurgeOpenFailureIsExitOne) {
    const std::string file_path = base_ + "/not_a_dir";
    { std::ofstream(file_path) << "x"; }
    const std::string cfg = WriteConfig(file_path);
    Argv a({"cortrix-server", "purge", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 1);
}

// --- gc-run open-failure → exit 1 (the run/purge branch's open-error arm) -------
TEST_F(GcCliFull, GcRunOpenFailureIsExitOne) {
    const std::string file_path = base_ + "/also_not_a_dir";
    { std::ofstream(file_path) << "y"; }
    const std::string cfg = WriteConfig(file_path);
    Argv a({"cortrix-server", "gc-run", "--config", cfg});
    auto rc = MaybeRunGcCli(a.argc(), a.argv());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 1);
}

}  // namespace
}  // namespace cortrix::server
