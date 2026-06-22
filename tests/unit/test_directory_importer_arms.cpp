// CLOSURE90 coverage-arm unit tests for src/connector/directory_importer.cpp.
//
// These target deterministically-reachable branches that the existing
// test_directory_importer.cpp / test_connector_coverage_gaps.cpp do NOT exercise,
// without modifying the shared NsPoolHarness (real per-NS store + FakeIndex):
//
//   InitialScan:
//     - ShouldTraverse(dir)==false -> disable_recursion_pending (ignored subdir
//       such as ".hidden": files inside are never scanned)            [lines 128-130]
//     - a non-regular-file entry (FIFO) is skipped                    [lines 135-137]
//     - error_threshold reached -> scan aborts early                  [lines 171-176]
//     - scan_batch_size progress-log branch (batch_size=1)            [lines 182-188]
//
//   ProcessFile:
//     - content-changed update whose SPC resubmit FAILS -> WARN arm   [lines 380-383]
//
//   HandleFileEvents:
//     - merge "else" arm (Deleted-then-Created on same path -> Created)[line 422]
//
//   HandleFileDeletion:
//     - Deleted event for a path with no matching doc (rc==-2) -> DEBUG
//       early return, deleted stat unchanged                          [lines 446-448]
//
// INTENTIONALLY NOT COVERED (require an injectable store/index failure that the
// shared harness does not provide — see the note in test_connector_coverage_gaps):
//   ComputeFileHash failure (274-278), doc_create failure (302-306),
//   doc_find_by_source other-error (391-394), kDeleting set-failure (460-462),
//   vector MarkDelete failure (472-478, FakeIndex.MarkDelete always Ok),
//   doc_delete failure (483-484), Start() canonical failure (63),
//   DeleteAllDocs per-path failure WARN (535).
//
// Suite name module-prefixed + globally unique. Temp dirs are getpid()+counter
// unique so parallel (-j) test binaries never collide / mutually delete.
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>  // mkfifo for the non-regular-file scan arm

#include "cortrix/connector/directory_importer.h"
#include "cortrix/config/config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/spc_manager.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;

std::string ArmsUniqueSuffix() {
    static std::atomic<int> counter{0};
    return std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1));
}

void WriteArmsFile(const fs::path& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// SPC stub that records Submit / Cancel and can be told to fail Submit for
// is_update tasks (drives the update-resubmit WARN arm).
class ArmsSPC : public SPCManager {
public:
    ArmsSPC() : SPCManager() {}
    Status Submit(std::shared_ptr<SPCTask> t) override {
        submitted.push_back(t);
        if (fail_updates && t->is_update) {
            return Status::Internal("forced update submit failure");
        }
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

    bool fail_updates = false;
    std::vector<std::shared_ptr<SPCTask>> submitted;
    std::vector<std::string> cancelled;
};

class DirectoryImporterArms : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = fs::temp_directory_path() / ("cortrix_diarm_" + ArmsUniqueSuffix());
        fs::create_directories(base_);
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(base_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());
        watch_dir_ = base_ / "watch";
        fs::create_directories(watch_dir_);
        spc_ = std::make_unique<ArmsSPC>();
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
        cfg.allowed_extensions = {};  // allow all extensions
        // Keep the importer's own ignore patterns (so ".hidden" dir is ignored).
        return cfg;
    }

    // Mark every kPending doc kReady (what a successful SPC worker would do).
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
    std::unique_ptr<ArmsSPC> spc_;
};

// --- InitialScan: an ignored subdirectory disables recursion into it ---

TEST_F(DirectoryImporterArms, ScanSkipsTraversalOfIgnoredSubdir) {
    // ".hidden" matches the default ignore pattern ".*" -> ShouldTraverse==false,
    // so disable_recursion_pending() fires and the file inside is never counted.
    fs::path hidden = watch_dir_ / ".hidden";
    fs::create_directories(hidden);
    WriteArmsFile(hidden / "buried.txt", "should not be scanned");
    WriteArmsFile(watch_dir_ / "top.txt", "scanned");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    // Only the top-level file is imported; the buried one is never visited.
    EXPECT_EQ(di.GetStats().imported, 1);
    ASSERT_EQ(spc_->submitted.size(), 1u);
}

// --- InitialScan: a non-regular file (FIFO) is skipped ---

TEST_F(DirectoryImporterArms, ScanSkipsNonRegularFile) {
    fs::path fifo = watch_dir_ / "a_pipe";
    // mkfifo creates a named pipe: exists, not a directory, not a regular file.
    ASSERT_EQ(0, ::mkfifo(fifo.c_str(), 0644)) << "mkfifo failed";
    WriteArmsFile(watch_dir_ / "regular.txt", "plain");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    // The FIFO is skipped at the is_regular_file() guard; only the .txt imports.
    EXPECT_EQ(di.GetStats().imported, 1);
}

// --- InitialScan: error_threshold reached -> scan aborts ---

TEST_F(DirectoryImporterArms, ScanAbortsWhenErrorThresholdReached) {
    // Bind to a never-admitted namespace so EVERY ProcessFile hits an acquire
    // failure (skipped_error++). With error_threshold=1 the scan breaks after the
    // first consecutive error instead of processing every file.
    for (int i = 0; i < 5; ++i) {
        WriteArmsFile(watch_dir_ / ("f" + std::to_string(i) + ".txt"), "x");
    }
    auto cfg = MakeCfg("never_admitted_ns");
    cfg.error_threshold = 1;
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    // At least one error was counted and the abort fired before all 5 ran; the
    // exact count is timing-of-iteration independent only as a lower bound, but
    // the abort caps it well below total_files.
    EXPECT_GE(di.GetStats().skipped_error, 1);
    EXPECT_LT(di.GetStats().skipped_error, 5);
    EXPECT_EQ(di.GetStats().imported, 0);
}

// --- InitialScan: scan_batch_size progress-log branch ---

TEST_F(DirectoryImporterArms, ScanBatchProgressLogBranch) {
    // scan_batch_size=1 -> the "processed % batch == 0" progress-log branch runs
    // for every processed file. Two healthy files both import.
    WriteArmsFile(watch_dir_ / "p1.txt", "one");
    WriteArmsFile(watch_dir_ / "p2.txt", "two");

    auto cfg = MakeCfg();
    cfg.scan_batch_size = 1;
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    EXPECT_EQ(di.GetStats().imported, 2);
}

// --- ProcessFile: content-changed update whose SPC resubmit FAILS ---

TEST_F(DirectoryImporterArms, UpdateResubmitFailureTakesWarnArm) {
    fs::path file = watch_dir_ / "doc.txt";
    WriteArmsFile(file, "version one");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    ASSERT_EQ(di.GetStats().imported, 1);
    ASSERT_EQ(MarkAllDocsReady(), 1);

    // Make the SPC fail on the update resubmit, then change the content so the
    // rescan takes the update arm and hits the submit-failure WARN.
    spc_->fail_updates = true;
    WriteArmsFile(file, "version two is meaningfully longer");
    di.Stop();
    ASSERT_TRUE(di.Start().ok());

    // The update path still counts the update and cancels the in-flight task even
    // though the resubmit failed (best-effort: WARN, no stat rollback).
    EXPECT_EQ(di.GetStats().updated, 1);
    EXPECT_FALSE(spc_->cancelled.empty());
    ASSERT_FALSE(spc_->submitted.empty());
    EXPECT_TRUE(spc_->submitted.back()->is_update);
}

// --- HandleFileEvents: merge "else" arm (Deleted then Created -> Created) ---

TEST_F(DirectoryImporterArms, EventMergeDeletedThenCreatedReimports) {
    fs::path file = watch_dir_ / "reborn.txt";
    WriteArmsFile(file, "content");
    std::string canon = fs::canonical(file).string();

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());  // running, no docs yet for this path-on-events

    int64_t imported_before = di.GetStats().imported;

    // Same path: Deleted first, then Created. The merge map sees prev=Deleted,
    // curr=Created -> the catch-all "else" arm sets it to Created, so the final
    // action is ProcessFile (re-import), not a deletion.
    std::vector<FileEvent> events;
    FileEvent del;
    del.type = FileEventType::kDeleted;
    del.path = canon;
    del.is_directory = false;
    events.push_back(del);
    FileEvent crt;
    crt.type = FileEventType::kCreated;
    crt.path = canon;
    crt.is_directory = false;
    events.push_back(crt);
    di.HandleFileEvents(events);

    // The file already imported during Start()'s scan, so the re-Created event
    // sees an unchanged doc; the key assertion is that the deletion was NOT taken
    // (deleted stays 0) -- i.e. the else-arm collapsed to Created.
    EXPECT_EQ(di.GetStats().deleted, 0);
    // imported is unchanged-or-greater (unchanged doc -> skipped_unchanged), never
    // a deletion path.
    EXPECT_GE(di.GetStats().imported, imported_before);
}

// --- HandleFileDeletion: Deleted event for a path with no doc (rc==-2) ---

TEST_F(DirectoryImporterArms, DeletionOfUnknownPathIsNoOp) {
    auto cfg = MakeCfg();  // admitted ns so acquire SUCCEEDS (reach the doc_find)
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    int64_t deleted_before = di.GetStats().deleted;
    FileEvent ev;
    ev.type = FileEventType::kDeleted;
    ev.path = (watch_dir_ / "never_indexed.txt").string();
    ev.is_directory = false;
    di.HandleFileEvents({ev});  // acquire ok, doc_find rc==-2 -> early Ok return

    EXPECT_EQ(di.GetStats().deleted, deleted_before);
}

}  // namespace
}  // namespace cortrix
