// Coverage-gap unit tests for src/connector/directory_importer.cpp (62%) and a
// small non-thread dedup case for dir_watcher_registry.cpp.
//
// test_directory_importer.cpp already covers Start/Stop/GetStats, the event-merge
// matrix, and DeleteAllDocs. This file targets the remaining ProcessFile /
// HandleFileDeletion / CleanupStaleDocs branches that are deterministically
// reachable with the shared NsPoolHarness (real per-NS store + FakeIndex):
//
//   ProcessFile:
//     - path-resolution failure (Created event for a path not on disk)
//       -> ResolvePath returns "" -> skipped_error++
//     - namespace-acquire failure (importer bound to a never-admitted NS)
//       -> skipped_error++
//     - content-changed update path: re-scan after the file content changes
//       -> CancelBySourcePath + kStale->kPending + SPC resubmit with is_update=true
//
//   HandleFileDeletion (via a Deleted event):
//     - namespace-acquire failure -> NotFound, deleted stat unchanged
//     - happy cascade for a doc that exists -> deleted++
//
//   CleanupStaleDocs (via a re-Start initial scan):
//     - a doc whose file was removed from disk while "offline" is purged on rescan
//
//   dir_watcher_registry:
//     - Subscribe is idempotent per-namespace, Unsubscribe of the only NS tears
//       the slot down (non-thread bookkeeping, no OS watcher).
//
// NOTE (intentionally NOT covered here): the HandleFileDeletion failure cascades
// the spec lists -- vector MarkDelete failure, doc_delete failure, kDeleting
// status set-failure -- require a *store/index that can be told to fail*. The
// shared NsPoolHarness uses a real store and a FakeIndex whose MarkDelete always
// succeeds, with no injectable failure seam, so those arms are not reachable from
// here without modifying the shared helper (out of scope: do-not-touch).
//
// Suite names module-prefixed + unique. Temp dirs are getpid()+counter unique.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_types.h"  // NSMetadata (router CreateNamespace)
#include "cortrix/connector/dir_watcher_registry.h"
#include "cortrix/connector/directory_importer.h"
#include "cortrix/connector/file_watcher.h"
#include "cortrix/config/config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/spc_manager.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;

// Process-unique suffix so parallel test binaries never collide on a temp dir.
std::string UniqueSuffix() {
    static std::atomic<int> counter{0};
    return std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1));
}

// SPCManager stub that records Submit + Cancel calls (mirrors the one in
// test_directory_importer.cpp; kept local to avoid cross-file coupling).
class GapRecordingSPC : public SPCManager {
public:
    GapRecordingSPC() : SPCManager() {}
    Status Submit(std::shared_ptr<SPCTask> t) override {
        submitted.push_back(t);
        return Status::Ok();
    }
    int CancelBySourcePath(const std::string& p) override {
        cancelled.push_back(p);
        return 0;
    }
    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return 0; }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }

    std::vector<std::shared_ptr<SPCTask>> submitted;
    std::vector<std::string> cancelled;
};

void WriteFile(const fs::path& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

class ConnectorGapTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = fs::temp_directory_path() / ("cortrix_congap_" + UniqueSuffix());
        fs::create_directories(base_);
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(base_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());
        watch_dir_ = base_ / "watch";
        fs::create_directories(watch_dir_);
        spc_ = std::make_unique<GapRecordingSPC>();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    WatchDirConfig MakeCfg(const std::string& ns = "default") const {
        WatchDirConfig cfg;
        cfg.data_dir = watch_dir_.string();
        cfg.namespace_name = ns;
        cfg.watch_enabled = true;
        cfg.allowed_extensions = {};  // allow all
        return cfg;
    }

    // Mark every kPending doc kReady (what a successful SPC worker would do), so a
    // re-scan treats an unchanged doc as unchanged rather than re-enqueuing it.
    int MarkAllDocsReady(const std::string& ns = "default") {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), ns);
        if (!facade.Acquire().ok()) return -1;
        std::vector<CortrixDoc> pending;
        facade.store().doc_list_by_status(DocStatus::kPending, pending);
        for (const auto& d : pending) {
            facade.store().doc_update_status(d.doc_id, DocStatus::kReady);
        }
        return static_cast<int>(pending.size());
    }

    fs::path base_;
    fs::path watch_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<GapRecordingSPC> spc_;
};

// --- ProcessFile: path resolution failure ---

TEST_F(ConnectorGapTest, ProcessFilePathResolutionFailureCountsError) {
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    int64_t err_before = di.GetStats().skipped_error;
    size_t sub_before = spc_->submitted.size();

    // Created event for a .txt path that does not exist on disk: passes the
    // extension filter, but ResolvePath (fs::canonical) fails -> skipped_error++.
    FileEvent ev;
    ev.type = FileEventType::kCreated;
    ev.path = (watch_dir_ / "ghost_never_created.txt").string();
    ev.is_directory = false;
    di.HandleFileEvents({ev});

    EXPECT_GT(di.GetStats().skipped_error, err_before);
    EXPECT_EQ(spc_->submitted.size(), sub_before);  // nothing submitted
}

// --- ProcessFile: namespace acquire failure ---

TEST_F(ConnectorGapTest, ProcessFileNamespaceAcquireFailureCountsError) {
    // Importer bound to a namespace that was never admitted -> facade.Acquire()
    // fails after the path resolves -> skipped_error++.
    WriteFile(watch_dir_ / "real.txt", "content");
    auto cfg = MakeCfg("never_admitted_ns");
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    // Start() runs an initial scan: every file hits the acquire failure.
    ASSERT_TRUE(di.Start().ok());

    EXPECT_GE(di.GetStats().skipped_error, 1);
    EXPECT_EQ(di.GetStats().imported, 0);
    EXPECT_TRUE(spc_->submitted.empty());
}

// --- ProcessFile: content-changed update path (SPC resubmit, is_update=true) ---

TEST_F(ConnectorGapTest, ProcessFileContentChangedTriggersUpdateResubmit) {
    fs::path file = watch_dir_ / "doc.txt";
    WriteFile(file, "version one");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    ASSERT_EQ(di.GetStats().imported, 1);
    ASSERT_EQ(MarkAllDocsReady(), 1);  // simulate the SPC worker completing

    size_t sub_before = spc_->submitted.size();
    size_t cancel_before = spc_->cancelled.size();

    // Change the file content -> hash differs -> the rescan takes the update arm.
    WriteFile(file, "version two is longer than one");
    di.Stop();
    ASSERT_TRUE(di.Start().ok());

    EXPECT_EQ(di.GetStats().updated, 1);
    ASSERT_GT(spc_->submitted.size(), sub_before);
    // The update path cancels any in-flight task for the path and resubmits with
    // is_update=true + an old_doc_id pointer.
    EXPECT_GT(spc_->cancelled.size(), cancel_before);
    const auto& task = spc_->submitted.back();
    EXPECT_TRUE(task->is_update);
    EXPECT_EQ(task->namespace_name, "default");
}

// --- HandleFileDeletion: namespace acquire failure (via a Deleted event) ---

TEST_F(ConnectorGapTest, DeletionNamespaceAcquireFailureLeavesStatsUnchanged) {
    auto cfg = MakeCfg("never_admitted_ns_del");
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    // No Start() scan needed; drive a Deleted event straight through.
    int64_t deleted_before = di.GetStats().deleted;

    FileEvent ev;
    ev.type = FileEventType::kDeleted;
    ev.path = (watch_dir_ / "anything.txt").string();
    ev.is_directory = false;
    di.HandleFileEvents({ev});  // acquire fails -> HandleFileDeletion returns early

    EXPECT_EQ(di.GetStats().deleted, deleted_before);  // no cascade ran
}

// --- HandleFileDeletion: happy cascade for an existing doc ---

TEST_F(ConnectorGapTest, DeletionCascadeDeletesExistingDoc) {
    WriteFile(watch_dir_ / "to_delete.txt", "payload");
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    ASSERT_EQ(di.GetStats().imported, 1);

    // Capture the canonical path (ProcessFile stored the doc under it) before
    // removing the file from disk.
    std::string canon = fs::canonical(watch_dir_ / "to_delete.txt").string();
    fs::remove(canon);

    int64_t deleted_before = di.GetStats().deleted;
    FileEvent ev;
    ev.type = FileEventType::kDeleted;
    ev.path = canon;
    ev.is_directory = false;
    di.HandleFileEvents({ev});

    EXPECT_GT(di.GetStats().deleted, deleted_before);
    // The cascade cancels any in-flight SPC task for the deleted path.
    EXPECT_FALSE(spc_->cancelled.empty());
}

// --- CleanupStaleDocs: a file removed while "offline" is purged on rescan ---

TEST_F(ConnectorGapTest, RescanCleansUpStaleDoc) {
    WriteFile(watch_dir_ / "keep.txt", "keep me");
    WriteFile(watch_dir_ / "stale.txt", "delete me later");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    ASSERT_EQ(di.GetStats().imported, 2);
    ASSERT_EQ(MarkAllDocsReady(), 2);

    // Remove one file from disk, then re-Start: InitialScan -> CleanupStaleDocs
    // notices the doc whose source_path no longer exists and purges it.
    fs::remove(watch_dir_ / "stale.txt");
    int64_t deleted_before = di.GetStats().deleted;
    di.Stop();
    ASSERT_TRUE(di.Start().ok());

    EXPECT_GT(di.GetStats().deleted, deleted_before);  // stale doc removed
}

// --- dir_watcher_registry: per-NS dedup + last-NS teardown (non-thread) ---

TEST_F(ConnectorGapTest, RegistrySubscribeDedupAndUnsubscribeTeardown) {
    // Wire the registry's CreateNamespace through the harness so a subscribed NS
    // is genuinely admitted (mirrors production); bookkeeping under test does not
    // depend on it, but it keeps the importer scans non-fatal.
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router;
    ON_CALL(ns_router, CreateNamespace(::testing::_))
        .WillByDefault(::testing::Invoke(
            [this](const cortrix::catalog::NSMetadata& m) {
                return harness_->Admit(m.namespace_id);
            }));

    DirWatcherRegistry reg(harness_->ipool(), ns_router, *spc_);
    std::string dir = watch_dir_.string();

    ASSERT_TRUE(reg.Subscribe(dir, {"team_a"}, true, /*autostart=*/false).ok());
    // Re-subscribe the same NS: idempotent, no new subscriber.
    ASSERT_TRUE(reg.Subscribe(dir, {"team_a"}, true, /*autostart=*/false).ok());
    {
        auto watches = reg.ListWatches();
        ASSERT_EQ(watches.size(), 1u);
        EXPECT_EQ(watches[0].subscriber_count, 1);
    }

    // Adding a second NS reuses the same watcher slot.
    ASSERT_TRUE(reg.Subscribe(dir, {"team_a", "team_b"}, true, false).ok());
    {
        auto watches = reg.ListWatches();
        ASSERT_EQ(watches.size(), 1u);
        EXPECT_EQ(watches[0].subscriber_count, 2);
    }

    // Unsubscribe one NS: slot survives with one subscriber.
    ASSERT_TRUE(reg.Unsubscribe(dir, "team_a").ok());
    EXPECT_EQ(reg.ListWatches().size(), 1u);

    // Unsubscribe the last NS: slot is destroyed.
    ASSERT_TRUE(reg.Unsubscribe(dir, "team_b").ok());
    EXPECT_TRUE(reg.ListWatches().empty());
}

}  // namespace
}  // namespace cortrix
