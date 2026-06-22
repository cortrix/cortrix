// CLOSURE90 coverage-arm unit tests for src/connector/dir_watcher_registry.cpp.
//
// These target deterministically-reachable lines that the existing
// test_dir_watcher_registry.cpp / test_connector_coverage_gaps.cpp do NOT cover,
// using only the public registry API (no private friend access needed):
//
//   Subscribe:
//     - CreateNamespace failure (non-AlreadyExists) -> WARN, still wires the
//       subscription                                                  [lines 257-262]
//
//   StartAll (no existing coverage at all):
//     - starts each importer + the OS watcher for every slot          [lines 453-471]
//
//   SaveState:
//     - empty data_dir -> InvalidArgument                             [lines 486-487]
//
//   LoadState:
//     - empty data_dir -> InvalidArgument                             [lines 516-517]
//     - v2 watcher with empty target_namespaces (degenerate) skipped  [lines 578-580]
//     - restore Subscribe failure (vanished dir) -> WARN, restored++ skipped [line 606]
//
// INTENTIONALLY NOT COVERED here (need failure injection / OS timing the shared
// harness does not provide):
//   MakeId OpenSSL fallback (87-93), FanOut importer-throw catch (165-166),
//   EnsureWatcherStarted Init/Start failure arms (181-196 partial),
//   GetStats ns/importer mismatch (407), TriggerScan importer-fail WARN (446-448),
//   SaveState ofstream-throw (509-511), autostart importer-Start-fail WARN
//   (277-279, importer Start never fails on an existing dir).
//
// Suite name module-prefixed + globally unique. Temp dirs are getpid()+counter
// unique so parallel (-j) test binaries never collide.
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/catalog_types.h"
#include "cortrix/connector/dir_watcher_registry.h"
#include "cortrix/spc/spc_pipeline.h"  // complete type for MockSPCManager dtor
#include "mock_spc_manager.h"
#include "ns_pool_test_helper.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

namespace cortrix {
namespace {

using cortrix::testing::MockSPCManager;

std::string RegArmsUniqueSuffix() {
    static std::atomic<int> counter{0};
    return std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1));
}

class DirWatcherRegistryArms : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        base_ = fs::temp_directory_path() / ("cortrix_regarm_" + RegArmsUniqueSuffix());
        fs::create_directories(base_);
        watch_dir_ = base_ / "watch";
        fs::create_directories(watch_dir_);
        { std::ofstream(watch_dir_ / "a.txt") << "alpha"; }
        canonical_dir_ = fs::canonical(watch_dir_).string();

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(base_ / "pool");

        // Default: CreateNamespace admits into the harness pool (production-like).
        ON_CALL(ns_router_, CreateNamespace(_))
            .WillByDefault(Invoke([this](const cortrix::catalog::NSMetadata& m) {
                return harness_->Admit(m.namespace_id);
            }));
        ON_CALL(spc_, Submit(_)).WillByDefault(Return(Status::Ok()));
    }
    void TearDown() override {
        harness_.reset();
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    cortrix::resource::INamespacePool& pool() { return harness_->ipool(); }
    cortrix::catalog::INSRouter& router() { return ns_router_; }

    std::filesystem::path base_;
    std::filesystem::path watch_dir_;
    std::string canonical_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router_;
    ::testing::NiceMock<MockSPCManager> spc_;
};

// --- Subscribe: CreateNamespace failure is non-fatal (WARN arm) ---

TEST_F(DirWatcherRegistryArms, SubscribeCreateNamespaceFailureIsNonFatal) {
    // Force CreateNamespace to return a hard error (not AlreadyExists): the
    // subscription must still be wired (WARN-and-continue), so the slot exists.
    EXPECT_CALL(ns_router_, CreateNamespace(_))
        .WillRepeatedly(Return(Status::Internal("forced create failure")));

    DirWatcherRegistry reg(pool(), router(), spc_);
    Status s = reg.Subscribe(watch_dir_.string(), {"team_a"}, true, /*autostart=*/false);
    ASSERT_TRUE(s.ok()) << s.message();

    auto watches = reg.ListWatches();
    ASSERT_EQ(1u, watches.size());
    EXPECT_EQ(1, watches[0].subscriber_count);
    EXPECT_EQ(canonical_dir_, watches[0].directory);
}

// --- StartAll: starts importers + OS watcher for every slot ---

TEST_F(DirWatcherRegistryArms, StartAllStartsImportersAndWatcher) {
    DirWatcherRegistry reg(pool(), router(), spc_);
    // Subscribe with autostart=false: no importer scan, no OS watcher yet.
    ASSERT_TRUE(reg.Subscribe(watch_dir_.string(), {"team_a", "team_b"},
                              true, /*autostart=*/false).ok());
    {
        auto before = reg.ListWatches();
        ASSERT_EQ(1u, before.size());
        EXPECT_FALSE(before[0].watching);  // OS watcher not started yet
    }

    // StartAll runs each importer's initial scan + EnsureWatcherStarted for the
    // slot (real FSEvents/inotify watcher created here).
    reg.StartAll();
    {
        auto after = reg.ListWatches();
        ASSERT_EQ(1u, after.size());
        EXPECT_TRUE(after[0].watching);  // OS watcher now running
    }

    // Idempotent: a second StartAll is a no-op (watcher already running).
    reg.StartAll();
    EXPECT_TRUE(reg.ListWatches()[0].watching);

    reg.StopAll();  // tear down the OS watcher cleanly before the dtor.
}

// --- SaveState: empty data_dir -> InvalidArgument ---

TEST_F(DirWatcherRegistryArms, SaveStateEmptyDataDirRejected) {
    DirWatcherRegistry reg(pool(), router(), spc_);
    Status s = reg.SaveState("");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(StatusCode::kInvalidArgument, s.code());
}

// --- LoadState: empty data_dir -> InvalidArgument ---

TEST_F(DirWatcherRegistryArms, LoadStateEmptyDataDirRejected) {
    DirWatcherRegistry reg(pool(), router(), spc_);
    Status s = reg.LoadState("");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(StatusCode::kInvalidArgument, s.code());
}

// --- LoadState: v2 watcher with empty target_namespaces is skipped ---

TEST_F(DirWatcherRegistryArms, LoadStateV2EmptyNamespacesSkipped) {
    namespace fs = std::filesystem;
    auto state_dir = base_ / "state_empty_ns";
    fs::create_directories(state_dir);

    // v2 file: one watcher whose target_namespaces is an empty array. The loader
    // takes the `nss.empty()` degenerate branch (add(dir,"",...)) and then refuses
    // to subscribe a dir with no namespaces.
    nlohmann::json j;
    j["version"] = 2;
    j["watchers"] = nlohmann::json::array();
    nlohmann::json w;
    w["directory"] = watch_dir_.string();
    w["recursive"] = true;
    w["target_namespaces"] = nlohmann::json::array();  // empty
    j["watchers"].push_back(w);
    { std::ofstream(state_dir / "watchers.json") << j.dump(2); }

    DirWatcherRegistry reg(pool(), router(), spc_);
    Status s = reg.LoadState(state_dir.string());
    EXPECT_TRUE(s.ok());  // non-fatal
    EXPECT_TRUE(reg.ListWatches().empty());  // degenerate watcher not subscribed
}

// --- LoadState: a restore whose Subscribe fails (vanished dir) -> WARN arm ---

TEST_F(DirWatcherRegistryArms, LoadStateRestoreSubscribeFailureIsNonFatal) {
    namespace fs = std::filesystem;
    auto state_dir = base_ / "state_vanished";
    fs::create_directories(state_dir);

    // v2 file referencing a directory that does NOT exist on disk -> Subscribe()
    // returns NotFound during restore -> the WARN-and-skip arm runs, restored
    // stays 0, LoadState still returns Ok.
    nlohmann::json j;
    j["version"] = 2;
    j["watchers"] = nlohmann::json::array();
    nlohmann::json w;
    w["directory"] = (base_ / "gone_forever").string();  // never created
    w["recursive"] = true;
    w["target_namespaces"] = std::vector<std::string>{"team_a"};
    j["watchers"].push_back(w);
    { std::ofstream(state_dir / "watchers.json") << j.dump(2); }

    DirWatcherRegistry reg(pool(), router(), spc_);
    Status s = reg.LoadState(state_dir.string());
    EXPECT_TRUE(s.ok());                      // non-fatal despite the failed restore
    EXPECT_TRUE(reg.ListWatches().empty());   // nothing actually restored
}

}  // namespace
}  // namespace cortrix
