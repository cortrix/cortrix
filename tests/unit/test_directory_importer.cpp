// Unit tests for DirectoryImporter (src/connector/directory_importer.cpp).
// Current coverage: 54.8%.  Targets the following untested/low-coverage paths:
//
//   Start() errors:
//     - empty data_dir → Ok (disabled path)
//     - non-existent data_dir → NotFound
//     - path exists but is not a directory → InvalidArgument
//     - valid dir → Ok, running_ = true
//
//   Stop() / IsWatching():
//     - Stop() after Start() → IsWatching() = false
//     - Stop() is idempotent
//
//   GetStats():
//     - initial zero stats
//     - stats updated after scan
//
//   HandleFileEvents() — event merging logic:
//     - directory events are filtered out
//     - file filtered by extension → skipped
//     - Created event → ProcessFile called
//     - Deleted event → HandleFileDeletion called
//     - Created then Deleted → merged out (temp file, ignored)
//     - Created then Modified → treated as Created
//     - Modified then Deleted → treated as Deleted
//
//   DeleteAllDocs():
//     - namespace acquire failure (bad ns) → returns -1
//     - no docs → returns 0
//     - docs exist → returns > 0
//
//   ProcessFile() via InitialScan():
//     - files with allowed extensions are imported
//     - files whose namespace acquire fails → skipped_error++
//     - error_threshold breaches abort the scan
//
// External dependencies (real ONNX, real LLM) are not touched.
// Uses NsPoolHarness for real per-namespace storage + real SPC stub.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/common/block_types.h"
#include "cortrix/connector/directory_importer.h"
#include "cortrix/config/config.h"
#include "cortrix/connector/file_watcher.h"
#include "cortrix/resource/namespace_facade.h"   // mark imported docs kReady in tests
#include "cortrix/spc/spc_manager.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;

// Minimal SPCManager stub that records Submit calls.
class RecordingSPC : public SPCManager {
public:
    RecordingSPC() : SPCManager() {}
    Status Submit(std::shared_ptr<SPCTask> t) override {
        submitted_tasks.push_back(t);
        calls.push_back("submit:" + t->doc_id);
        return Status::Ok();
    }
    int CancelQueuedVersions(const std::string& ns, const std::string& type,
                             const std::string& p) override {
        cancelled_paths.push_back(p);
        cancelled_scopes.push_back(ns + "|" + type);
        calls.push_back("cancel:" + p);
        return 0;
    }
    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return 0; }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    // Nothing runs here: a document is active only when a test says so. The hook runs
    // first, as a task that commits and finishes just then would.
    bool HasActiveTask(const std::string& doc_id) const override {
        if (on_has_active) on_has_active(doc_id);
        return active_ids.count(doc_id) > 0;
    }

    std::vector<std::shared_ptr<SPCTask>> submitted_tasks;
    std::vector<std::string> cancelled_paths;
    std::vector<std::string> cancelled_scopes;  // "namespace|source_type" per call
    std::vector<std::string> calls;             // "submit:<doc_id>" / "cancel:<path>", in order
    std::set<std::string> active_ids;
    std::function<void(const std::string&)> on_has_active;
};

// Write a small text file to `path`.
static void WriteFile(const fs::path& path, const std::string& content = "hello") {
    std::ofstream f(path);
    f << content;
}

// ---------------------------------------------------------------------------
// Fixture: owns a temp dir + NsPoolHarness + RecordingSPC
// ---------------------------------------------------------------------------

class DirectoryImporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = fs::temp_directory_path() /
                ("cortrix_ditest_" + std::to_string(::getpid()));
        fs::create_directories(base_);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(base_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        watch_dir_ = base_ / "watch";
        fs::create_directories(watch_dir_);

        spc_ = std::make_unique<RecordingSPC>();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    // Build a WatchDirConfig pointing at watch_dir_ for namespace "default".
    WatchDirConfig MakeCfg(const std::string& ns = "default") const {
        WatchDirConfig cfg;
        cfg.data_dir       = watch_dir_.string();
        cfg.namespace_name = ns;
        cfg.watch_enabled  = true;
        cfg.allowed_extensions = {};  // all extensions allowed
        return cfg;
    }

    // Build a WatchDirConfig with explicit data_dir (for error-path tests).
    WatchDirConfig MakeCfgRaw(const std::string& data_dir,
                               const std::string& ns = "default") const {
        WatchDirConfig cfg;
        cfg.data_dir       = data_dir;
        cfg.namespace_name = ns;
        return cfg;
    }

    // ProcessFile() creates docs in kPending and hands them to SPC for the heavy
    // pipeline. Our SPC stub never runs that pipeline, so docs stay kPending —
    // and a re-scan re-enqueues kPending/kError docs (crash-recovery path) instead
    // of treating them as unchanged. Mark every doc kReady (what a real SPC worker
    // would do on success) so a second scan exercises the skipped_unchanged path.
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
    std::unique_ptr<RecordingSPC> spc_;
};

// ---------------------------------------------------------------------------
// Start() — error paths
// ---------------------------------------------------------------------------

TEST_F(DirectoryImporterTest, StartEmptyDataDirReturnsOk) {
    // An empty data_dir means "disabled" — Start() returns Ok without doing anything.
    WatchDirConfig cfg;
    cfg.data_dir = "";
    cfg.namespace_name = "default";
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    Status s = di.Start();
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_FALSE(di.IsWatching());
}

TEST_F(DirectoryImporterTest, StartNonexistentDirReturnsNotFound) {
    auto cfg = MakeCfgRaw("/no/such/dir/xyz_999_never");
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    Status s = di.Start();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_NE(s.message().find("data_dir not found"), std::string::npos);
}

TEST_F(DirectoryImporterTest, StartRegularFileNotDirectoryReturnsInvalidArgument) {
    // Create a regular file and pass it as data_dir.
    fs::path file_path = base_ / "notadir.txt";
    WriteFile(file_path, "I am a file");
    auto cfg = MakeCfgRaw(file_path.string());
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    Status s = di.Start();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(DirectoryImporterTest, StartEmptyDirSucceedsAndIsWatching) {
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    Status s = di.Start();
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(di.IsWatching());
}

// ---------------------------------------------------------------------------
// Stop() / IsWatching()
// ---------------------------------------------------------------------------

TEST_F(DirectoryImporterTest, StopAfterStartSetsFlagFalse) {
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    EXPECT_TRUE(di.IsWatching());
    di.Stop();
    EXPECT_FALSE(di.IsWatching());
}

TEST_F(DirectoryImporterTest, StopIsIdempotent) {
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    di.Stop();
    di.Stop();  // second Stop() must not crash
    EXPECT_FALSE(di.IsWatching());
}

TEST_F(DirectoryImporterTest, StopBeforeStartIsIdempotent) {
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    EXPECT_NO_FATAL_FAILURE(di.Stop());
    EXPECT_FALSE(di.IsWatching());
}

// ---------------------------------------------------------------------------
// GetStats() — initial state + after scan
// ---------------------------------------------------------------------------

TEST_F(DirectoryImporterTest, InitialStatsAreZero) {
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    auto stats = di.GetStats();
    EXPECT_EQ(stats.total_files, 0);
    EXPECT_EQ(stats.imported, 0);
    EXPECT_EQ(stats.updated, 0);
    EXPECT_EQ(stats.skipped_unchanged, 0);
    EXPECT_EQ(stats.skipped_filtered, 0);
    EXPECT_EQ(stats.skipped_error, 0);
    EXPECT_EQ(stats.deleted, 0);
}

TEST_F(DirectoryImporterTest, StatsAfterScanCountFiles) {
    // Place two text files in the watch directory.
    WriteFile(watch_dir_ / "a.txt", "alpha");
    WriteFile(watch_dir_ / "b.txt", "beta");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    auto stats = di.GetStats();
    EXPECT_EQ(stats.total_files, 2);
    EXPECT_EQ(stats.imported, 2);
    EXPECT_EQ(stats.skipped_unchanged, 0);
}

TEST_F(DirectoryImporterTest, StatsElapsedSIsPositiveAfterScan) {
    WriteFile(watch_dir_ / "timing.txt");
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    EXPECT_GE(di.GetStats().elapsed_s, 0.0);
}

// ---------------------------------------------------------------------------
// Repeated scan: skipped_unchanged when file content does not change
// ---------------------------------------------------------------------------

TEST_F(DirectoryImporterTest, SecondStartSkipsUnchangedFile) {
    WriteFile(watch_dir_ / "stable.txt", "stable content");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    EXPECT_EQ(di.GetStats().imported, 1);

    // Simulate the SPC worker finishing: move the doc out of kPending to kReady.
    // Otherwise the re-scan would re-enqueue it (crash-recovery path) rather than
    // skip it as unchanged.
    ASSERT_EQ(MarkAllDocsReady(), 1);

    // Reset running flag (Stop + Start = re-scan).
    di.Stop();
    ASSERT_TRUE(di.Start().ok());
    auto stats = di.GetStats();
    // The second scan should skip the unchanged file.
    EXPECT_EQ(stats.skipped_unchanged, 1);
}

// ---------------------------------------------------------------------------
// Filtered extensions: skipped_filtered
// ---------------------------------------------------------------------------

TEST_F(DirectoryImporterTest, FilteredExtensionSkipped) {
    WriteFile(watch_dir_ / "note.txt");
    WriteFile(watch_dir_ / "data.csv");  // .csv not in allowed_extensions

    WatchDirConfig cfg = MakeCfg();
    cfg.allowed_extensions = {".txt"};  // only .txt allowed
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    auto stats = di.GetStats();
    EXPECT_EQ(stats.total_files, 2);
    // .csv is filtered; .txt is imported.
    EXPECT_GE(stats.skipped_filtered, 1);
    EXPECT_GE(stats.imported, 1);
}

// ---------------------------------------------------------------------------
// Subdirectory traversal
// ---------------------------------------------------------------------------

TEST_F(DirectoryImporterTest, SubdirFilesAreImported) {
    fs::path sub = watch_dir_ / "sub";
    fs::create_directories(sub);
    WriteFile(watch_dir_ / "root.txt", "root");
    WriteFile(sub / "child.txt", "child");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    EXPECT_EQ(di.GetStats().total_files, 2);
    EXPECT_EQ(di.GetStats().imported, 2);
}

// ---------------------------------------------------------------------------
// HandleFileEvents() — event merging
// ---------------------------------------------------------------------------

TEST_F(DirectoryImporterTest, DirectoryEventsAreIgnored) {
    WriteFile(watch_dir_ / "real.txt", "content");
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    size_t tasks_before = spc_->submitted_tasks.size();

    // A directory event must be ignored.
    FileEvent dir_ev;
    dir_ev.type         = FileEventType::kCreated;
    dir_ev.path         = (watch_dir_ / "new_subdir").string();
    dir_ev.is_directory = true;
    di.HandleFileEvents({dir_ev});

    EXPECT_EQ(spc_->submitted_tasks.size(), tasks_before);
}

TEST_F(DirectoryImporterTest, CreatedThenDeletedMergedToNoOp) {
    // A temp file: Created then Deleted in the same batch → event merge removes it.
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    size_t tasks_before = spc_->submitted_tasks.size();

    std::string path = (watch_dir_ / "tmp_gone.txt").string();
    FileEvent ev_create;
    ev_create.type         = FileEventType::kCreated;
    ev_create.path         = path;
    ev_create.is_directory = false;

    FileEvent ev_delete;
    ev_delete.type         = FileEventType::kDeleted;
    ev_delete.path         = path;
    ev_delete.is_directory = false;

    di.HandleFileEvents({ev_create, ev_delete});
    // Merged out: no SPC submit for a transient temp file.
    EXPECT_EQ(spc_->submitted_tasks.size(), tasks_before);
}

TEST_F(DirectoryImporterTest, ModifiedThenDeletedMergesAsDeleted) {
    // Pre-import a file so HandleFileDeletion can find it in the DB.
    WriteFile(watch_dir_ / "will_del.txt", "initial");
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    // ProcessFile stores the doc under the canonical (symlink-resolved) path, and
    // in production FSEvents reports paths under the canonicalized watch root, so
    // HandleFileDeletion looks up by the canonical path. Capture it while the file
    // still exists (fs::canonical needs a real file), then delete from disk.
    std::string path = fs::canonical(watch_dir_ / "will_del.txt").string();
    fs::remove(path);

    FileEvent ev_mod;
    ev_mod.type  = FileEventType::kModified;
    ev_mod.path  = path;
    FileEvent ev_del;
    ev_del.type  = FileEventType::kDeleted;
    ev_del.path  = path;

    int64_t deleted_before = di.GetStats().deleted;
    di.HandleFileEvents({ev_mod, ev_del});
    // The merged event should be kDeleted, triggering HandleFileDeletion.
    EXPECT_GT(di.GetStats().deleted, deleted_before);
}

// #178: the importer cancels queued work within its own namespace and producer only,
// at both of its call sites: a changed file and a deleted one.
TEST_F(DirectoryImporterTest, CancelsAreScopedToItsNamespaceAndWatchDir) {
    WriteFile(watch_dir_ / "scoped.txt", "first content");
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    ASSERT_EQ(MarkAllDocsReady(), 1);
    const std::string path = fs::canonical(watch_dir_ / "scoped.txt").string();

    // A changed file: the changed-content branch cancels the path's queued versions.
    WriteFile(watch_dir_ / "scoped.txt", "second content");
    FileEvent ev_mod;
    ev_mod.type = FileEventType::kModified;
    ev_mod.path = path;
    di.HandleFileEvents({ev_mod});
    ASSERT_EQ(spc_->cancelled_paths.size(), 1u);
    EXPECT_EQ(spc_->cancelled_paths[0], path);
    EXPECT_EQ(spc_->cancelled_scopes[0], "default|watch_dir");

    // A deleted file: the deletion handler cancels them as well.
    fs::remove(watch_dir_ / "scoped.txt");
    FileEvent ev_del;
    ev_del.type = FileEventType::kDeleted;
    ev_del.path = path;
    di.HandleFileEvents({ev_del});
    ASSERT_EQ(spc_->cancelled_paths.size(), 2u);
    EXPECT_EQ(spc_->cancelled_paths[1], path);
    EXPECT_EQ(spc_->cancelled_scopes[1], "default|watch_dir");
}

TEST_F(DirectoryImporterTest, CreatedThenModifiedTreatedAsCreated) {
    // Created + Modified in same batch → still one new import (not an update).
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    fs::path new_file = watch_dir_ / "created_mod.txt";
    WriteFile(new_file, "content v1");
    std::string path = new_file.string();

    size_t before = spc_->submitted_tasks.size();
    FileEvent ev_create;
    ev_create.type = FileEventType::kCreated;
    ev_create.path = path;
    FileEvent ev_mod;
    ev_mod.type  = FileEventType::kModified;
    ev_mod.path  = path;

    di.HandleFileEvents({ev_create, ev_mod});
    // One task submitted (new import, not an update from previous state).
    EXPECT_EQ(spc_->submitted_tasks.size(), before + 1);
}

TEST_F(DirectoryImporterTest, HandleFileEventsFilteredExtensionIgnored) {
    WatchDirConfig cfg = MakeCfg();
    cfg.allowed_extensions = {".txt"};
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());

    // A .log file should be filtered by ShouldImport.
    std::string path = (watch_dir_ / "ignored.log").string();
    WriteFile(fs::path(path), "log data");
    size_t before = spc_->submitted_tasks.size();

    FileEvent ev;
    ev.type = FileEventType::kCreated;
    ev.path = path;
    di.HandleFileEvents({ev});

    EXPECT_EQ(spc_->submitted_tasks.size(), before);
}

// ---------------------------------------------------------------------------
// DeleteAllDocs()
// ---------------------------------------------------------------------------

TEST_F(DirectoryImporterTest, DeleteAllDocsNonexistentNamespaceReturnsMinusOne) {
    // Using a namespace that was never admitted → acquire fails → -1.
    WatchDirConfig cfg = MakeCfgRaw(watch_dir_.string(), "never_admitted_ns");
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    int result = di.DeleteAllDocs();
    EXPECT_EQ(result, -1);
}

TEST_F(DirectoryImporterTest, DeleteAllDocsEmptyNamespaceReturnsZero) {
    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    // No Start(), so no documents in DB.
    int result = di.DeleteAllDocs();
    EXPECT_EQ(result, 0);
}

TEST_F(DirectoryImporterTest, DeleteAllDocsAfterImportDeletesAll) {
    WriteFile(watch_dir_ / "f1.txt", "content one");
    WriteFile(watch_dir_ / "f2.txt", "content two");

    auto cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    EXPECT_EQ(di.GetStats().imported, 2);

    int deleted = di.DeleteAllDocs();
    EXPECT_EQ(deleted, 2);
}

// ---------------------------------------------------------------------------
// ImportStats::ToString()
// ---------------------------------------------------------------------------

TEST(ImportStatsTest, ToStringContainsKeyFields) {
    ImportStats s;
    s.total_files = 10;
    s.imported    = 5;
    s.elapsed_s   = 1.23;
    std::string str = s.ToString();
    EXPECT_NE(str.find("total=10"), std::string::npos);
    EXPECT_NE(str.find("imported=5"), std::string::npos);
    EXPECT_NE(str.find("elapsed=1.23"), std::string::npos);
}


// --- #157: a changed file is a new version; deleting a file removes every live one ---

class DirectoryImporterVersionsTest : public DirectoryImporterTest {
protected:
    std::vector<CortrixDoc> LiveDocs() {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), "default");
        EXPECT_TRUE(facade.Acquire().ok());
        std::vector<CortrixDoc> all;
        for (auto st : {DocStatus::kReady, DocStatus::kPending, DocStatus::kProcessing,
                        DocStatus::kError, DocStatus::kStale, DocStatus::kDeleting}) {
            std::vector<CortrixDoc> batch;
            facade.store().doc_list_by_status(st, batch);
            for (auto& d : batch) all.push_back(std::move(d));
        }
        return all;
    }

    static FileEvent Event(FileEventType type, const std::string& path) {
        FileEvent ev;
        ev.type = type;
        ev.path = path;
        return ev;
    }

    std::string PathOf(const std::string& name) {
        return fs::canonical(watch_dir_ / name).string();
    }

    // The row the importer creates for a new file, and the task it submits for it.
    std::string Import(DirectoryImporter& di, const std::string& name,
                       const std::string& content) {
        WriteFile(watch_dir_ / name, content);
        const std::size_t before = spc_->submitted_tasks.size();
        di.HandleFileEvents({Event(FileEventType::kCreated, PathOf(name))});
        EXPECT_EQ(spc_->submitted_tasks.size(), before + 1) << name;
        return spc_->submitted_tasks.empty() ? std::string()
                                             : spc_->submitted_tasks.back()->doc_id;
    }

    void Exec(const std::string& sql) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), "default");
        ASSERT_TRUE(facade.Acquire().ok());
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(facade.store().db_handle(), sql.c_str(), nullptr, nullptr, &err),
                  SQLITE_OK)
            << (err ? err : "") << " :: " << sql;
        sqlite3_free(err);
    }

    void SetStatus(const std::string& doc_id, DocStatus status) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), "default");
        ASSERT_TRUE(facade.Acquire().ok());
        ASSERT_EQ(facade.store().doc_update_status(doc_id, status), 0);
    }

    // One stored block for doc_id, as a processing attempt leaves.
    void AddBlock(const std::string& doc_id) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), "default");
        ASSERT_TRUE(facade.Acquire().ok());
        CortrixBlock b;
        b.doc_id = doc_id;
        b.child_id = "child-" + std::to_string(++blocks_added_) + "-" + doc_id;
        b.block_type = static_cast<int>(kBlockFile);
        b.content_text = "stored attempt";
        b.metadata_json = "{}";
        b.data = {0x01};
        ASSERT_EQ(facade.store().block_insert(b), 0);
    }

    // What one more event for the file's unchanged content does.
    enum class Outcome { kSkipped, kInPlace, kNewVersion };
    Outcome Again(DirectoryImporter& di, const std::string& name, const std::string& doc_id) {
        const std::size_t before = spc_->submitted_tasks.size();
        di.HandleFileEvents({Event(FileEventType::kModified, PathOf(name))});
        if (spc_->submitted_tasks.size() == before) return Outcome::kSkipped;
        EXPECT_EQ(spc_->submitted_tasks.size(), before + 1) << name;
        return spc_->submitted_tasks.back()->doc_id == doc_id ? Outcome::kInPlace
                                                               : Outcome::kNewVersion;
    }

    int blocks_added_ = 0;
};

TEST_F(DirectoryImporterVersionsTest, ChangedFileGetsANewPendingVersion) {
    WriteFile(watch_dir_ / "v.txt", "first content");
    const WatchDirConfig cfg = MakeCfg();
    DirectoryImporter di(cfg, harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    ASSERT_EQ(MarkAllDocsReady(), 1);
    const std::vector<CortrixDoc> before = LiveDocs();
    ASSERT_EQ(before.size(), 1u);
    const CortrixDoc v1 = before[0];
    const std::string path = fs::canonical(watch_dir_ / "v.txt").string();

    const std::string second = "second, longer content";
    WriteFile(watch_dir_ / "v.txt", second);
    di.HandleFileEvents({Event(FileEventType::kModified, path)});

    const std::vector<CortrixDoc> after = LiveDocs();
    ASSERT_EQ(after.size(), 2u);
    const CortrixDoc* old = nullptr;
    const CortrixDoc* v2 = nullptr;
    for (const auto& d : after) (d.doc_id == v1.doc_id ? old : v2) = &d;
    ASSERT_NE(old, nullptr);
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(old->status, DocStatus::kReady);  // untouched until the new one is ready
    EXPECT_EQ(old->content_hash, v1.content_hash);
    EXPECT_EQ(v2->status, DocStatus::kPending);
    EXPECT_EQ(v2->source_type, "watch_dir");
    EXPECT_EQ(v2->source_path, path);
    EXPECT_FALSE(v1.source_ref.empty());
    EXPECT_EQ(v2->source_ref, v1.source_ref);  // the importer's resolved data dir
    EXPECT_EQ(v2->file_size, static_cast<int64_t>(second.size()));
    EXPECT_EQ(v2->mime_type, v1.mime_type);
    EXPECT_EQ(v2->title, "v.txt");
    EXPECT_EQ(v2->processing_level, 3);
    EXPECT_NE(v2->content_hash, v1.content_hash);

    ASSERT_FALSE(spc_->submitted_tasks.empty());
    const auto& task = spc_->submitted_tasks.back();
    EXPECT_EQ(task->doc_id, v2->doc_id);
    EXPECT_TRUE(task->is_update);
    EXPECT_EQ(task->old_doc_id, v1.doc_id);
}

TEST_F(DirectoryImporterVersionsTest, PendingVersionWithNoActiveTaskIsResubmittedInPlace) {
    WriteFile(watch_dir_ / "w.txt", "first content");
    DirectoryImporter di(MakeCfg(), harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    ASSERT_EQ(MarkAllDocsReady(), 1);
    const std::string path = fs::canonical(watch_dir_ / "w.txt").string();
    WriteFile(watch_dir_ / "w.txt", "second content");
    di.HandleFileEvents({Event(FileEventType::kModified, path)});  // v2 pending
    ASSERT_FALSE(spc_->submitted_tasks.empty());
    const std::string v2 = spc_->submitted_tasks.back()->doc_id;

    // No task for v2 is active and it holds no blocks, so the same content again is
    // processed in place, cancelling any task of the path that is still queued first.
    spc_->calls.clear();
    di.HandleFileEvents({Event(FileEventType::kModified, path)});
    ASSERT_EQ(spc_->calls.size(), 2u);
    EXPECT_EQ(spc_->calls[0], "cancel:" + path);
    EXPECT_EQ(spc_->calls[1], "submit:" + v2);  // the resubmitted task is v2's
}

// #157: an unchanged file whose version never finished is processed again, unless a
// task for it is queued or running. A version that holds blocks stored an attempt that
// was never published; processing it in place would collide with its document-level
// metadata block, so it gets a new version instead.
TEST_F(DirectoryImporterVersionsTest, UnchangedFileFollowsItsVersionsState) {
    DirectoryImporter di(MakeCfg(), harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    using O = Outcome;
    struct Case {
        const char* name;
        DocStatus status;
        bool blocks;
        bool active;
        Outcome expected;
    };
    const Case cases[] = {
        {"error-blocks.txt", DocStatus::kError, true, false, O::kNewVersion},
        {"error-empty.txt", DocStatus::kError, false, false, O::kInPlace},
        {"error-active.txt", DocStatus::kError, true, true, O::kSkipped},
        {"processing-blocks.txt", DocStatus::kProcessing, true, false, O::kNewVersion},
        {"processing-empty.txt", DocStatus::kProcessing, false, false, O::kInPlace},
        {"processing-active.txt", DocStatus::kProcessing, true, true, O::kSkipped},
        {"pending-empty.txt", DocStatus::kPending, false, false, O::kInPlace},
        {"pending-blocks.txt", DocStatus::kPending, true, false, O::kNewVersion},
        {"pending-active.txt", DocStatus::kPending, false, true, O::kSkipped},
        {"ready.txt", DocStatus::kReady, true, false, O::kSkipped},
    };
    for (const Case& c : cases) {
        const std::string id = Import(di, c.name, std::string("content of ") + c.name);
        if (c.status != DocStatus::kPending) SetStatus(id, c.status);
        if (c.blocks) AddBlock(id);
        if (c.active) spc_->active_ids.insert(id);
        EXPECT_EQ(static_cast<int>(Again(di, c.name, id)), static_cast<int>(c.expected))
            << c.name << " (0 skipped, 1 in place, 2 new version)";
        spc_->active_ids.erase(id);
    }
}

// A block count that cannot be read is taken to mean blocks, so the importer never
// processes a version that may hold content in place.
TEST_F(DirectoryImporterVersionsTest, AnUnreadableBlockCountMeansANewVersion) {
    DirectoryImporter di(MakeCfg(), harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    const std::string id = Import(di, "count.txt", "content");
    SetStatus(id, DocStatus::kError);
    Exec("ALTER TABLE blocks RENAME TO blocks_hidden");  // the count's prepare fails
    const Outcome outcome = Again(di, "count.txt", id);
    Exec("ALTER TABLE blocks_hidden RENAME TO blocks");
    EXPECT_EQ(static_cast<int>(outcome), static_cast<int>(Outcome::kNewVersion));
}

// The block count is read after the active check. A task that stores its blocks and
// finishes just as the importer asks leaves blocks the count then sees, so the version
// is not processed in place.
TEST_F(DirectoryImporterVersionsTest, TheBlockCountIsReadAfterTheActiveCheck) {
    DirectoryImporter di(MakeCfg(), harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    const std::string id = Import(di, "order.txt", "content");
    SetStatus(id, DocStatus::kProcessing);
    spc_->on_has_active = [&](const std::string& doc_id) {
        if (doc_id == id) AddBlock(id);
    };
    const Outcome outcome = Again(di, "order.txt", id);
    spc_->on_has_active = nullptr;
    EXPECT_EQ(static_cast<int>(outcome), static_cast<int>(Outcome::kNewVersion));
}

TEST_F(DirectoryImporterVersionsTest, DeletedFileRemovesEveryLiveVersion) {
    WriteFile(watch_dir_ / "d.txt", "first content");
    DirectoryImporter di(MakeCfg(), harness_->ipool(), *spc_);
    ASSERT_TRUE(di.Start().ok());
    ASSERT_EQ(MarkAllDocsReady(), 1);
    const std::string path = fs::canonical(watch_dir_ / "d.txt").string();
    WriteFile(watch_dir_ / "d.txt", "second content");
    di.HandleFileEvents({Event(FileEventType::kModified, path)});
    ASSERT_EQ(LiveDocs().size(), 2u);  // v1 ready, v2 pending

    const auto deleted_before = di.GetStats().deleted;
    fs::remove(watch_dir_ / "d.txt");
    di.HandleFileEvents({Event(FileEventType::kDeleted, path)});
    EXPECT_TRUE(LiveDocs().empty());
    EXPECT_EQ(di.GetStats().deleted, deleted_before + 1);
}

}  // namespace
}  // namespace cortrix
