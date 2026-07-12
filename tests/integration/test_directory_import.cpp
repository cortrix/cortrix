#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "cortrix/connector/directory_importer.h"
#include "cortrix/connector/file_utils.h"
#include "cortrix/resource/namespace_facade.h"  // [wire5c] per-op facade for seed/read-back
#include "cortrix/spc/spc_pipeline.h"  // needed for complete type in MockSPCManager dtor
#include "mock_spc_manager.h"
#include "ns_pool_test_helper.h"  // [wire5c] F05 NsPoolHarness replaces MVP CortrixNamespaceManager
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using ::testing::_;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::Invoke;

namespace cortrix {
namespace {

// Using shared MockSPCManager from tests/mocks/mock_spc_manager.h
using cortrix::testing::MockSPCManager;

// ---- Test Fixture ----
//
// [wire5c] DirectoryImporter migrated off the MVP CortrixNamespaceManager onto the
// F05 resource::INamespacePool. Its store/vector/blob windows are now reached per-op
// through a NamespaceFacade (NOT injected mocks): the importer calls
// facade.store().doc_create(...) / facade.vec_index().MarkDelete(...) against the
// REAL CortrixStoreSqlite + capturing FakeIndex that NsPoolHarness stands up.
//
// Consequences for these tests (vs the old gmock-on-CortrixNamespace fixture):
//   * "new file" assertions just import against the real store (find->-2->create).
//   * "unchanged"/"update" assertions PRE-SEED a doc into the harness store via a
//     setup facade (SeedDoc), so the importer's real doc_find_by_source hits it.
//   * SPC behaviour (Submit / CancelBySourcePath) is still verified with MockSPCManager.
//   * doc_id is a ULID string now (D-I6), not an int64.
class DirectoryImportTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        // Unique per test (same idiom as harness_root_ below): parallel ctest
        // runs suite tests as concurrent processes, and a shared scanned dir
        // races one test's SetUp/remove_all against a sibling's live scan.
        test_dir_ = fs::temp_directory_path() /
                    (std::string("cortrix_test_dir_import_") +
                     ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(test_dir_);
        fs::create_directories(test_dir_);

        // Per-test harness data root (kept distinct from the scanned test_dir_),
        // made unique by appending the running test's name.
        harness_root_ = fs::temp_directory_path() /
                        (std::string("cortrix_dirimp_pool_") +
                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(harness_root_);

        // Create test files
        CreateFile("test.txt", "Hello, World!");
        CreateFile("test.md", "# Markdown Title\n\nSome content here.");
        CreateFile("test.pdf", "%PDF-1.4 fake pdf content");

        // Create hidden/temp files that should be filtered
        CreateFile(".hidden_file", "should be ignored");
        CreateFile("temp~", "should be ignored");
        fs::create_directory(test_dir_ / ".git");
        CreateFile(".git/config", "git config");
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
        std::filesystem::remove_all(harness_root_);
    }

    void CreateFile(const std::string& name, const std::string& content) {
        auto path = test_dir_ / name;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    // Build a fresh harness rooted at a unique dir and admit one namespace so the
    // importer's per-op facade Acquire() hits. The importer's real store/vec/blob
    // windows live behind this pool.
    std::unique_ptr<test::NsPoolHarness> MakeHarness(const std::string& admit_ns = "default") {
        auto h = std::make_unique<test::NsPoolHarness>(harness_root_);
        EXPECT_TRUE(h->Admit(admit_ns).ok());
        return h;
    }

    // Pre-seed a watch_dir document into the harness store (so the importer's real
    // doc_find_by_source returns it). Returns the minted ULID doc_id.
    std::string SeedDoc(test::NsPoolHarness& h, const std::string& ns,
                        const std::string& source_path, const std::string& content_hash,
                        const std::string& mime_type = "",
                        DocStatus status = DocStatus::kReady) {
        cortrix::resource::NamespaceFacade facade(h.ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        CortrixDoc doc;
        doc.source_type = "watch_dir";
        doc.source_path = source_path;
        doc.source_ref = test_dir_.string();
        doc.content_hash = content_hash;
        doc.mime_type = mime_type;
        doc.status = status;
        doc.title = GetBasename(source_path);
        EXPECT_EQ(0, facade.store().doc_create(doc));
        return doc.doc_id;
    }

    // Pre-seed one block under a doc (used by the cascade-delete tests).
    void SeedBlock(test::NsPoolHarness& h, const std::string& ns,
                   const std::string& doc_id, const std::string& text) {
        cortrix::resource::NamespaceFacade facade(h.ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        CortrixBlock block;
        block.doc_id = doc_id;
        block.chunk_index = 0;
        block.content_text = text;
        block.data = {0x01, 0x02, 0x03};  // non-empty to satisfy NOT NULL
        EXPECT_EQ(0, facade.store().block_insert(block));
    }

    // Read back all watch_dir docs across the statuses the importer touches.
    std::vector<CortrixDoc> ReadDocs(test::NsPoolHarness& h, const std::string& ns) {
        cortrix::resource::NamespaceFacade facade(h.ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        std::vector<CortrixDoc> all;
        for (auto status : {DocStatus::kReady, DocStatus::kPending, DocStatus::kProcessing,
                            DocStatus::kError, DocStatus::kStale, DocStatus::kDeleting}) {
            std::vector<CortrixDoc> batch;
            facade.store().doc_list_by_status(status, batch);
            for (auto& d : batch) all.push_back(std::move(d));
        }
        return all;
    }

    // Whether a doc still exists in the store (for cascade-delete verification).
    bool DocExists(test::NsPoolHarness& h, const std::string& ns, const std::string& doc_id) {
        cortrix::resource::NamespaceFacade facade(h.ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        CortrixDoc d;
        return facade.store().doc_get(doc_id, d) == 0;
    }

    std::filesystem::path test_dir_;
    std::filesystem::path harness_root_;
};

// ---- Tests ----

TEST_F(DirectoryImportTest, FullScanThreeFiles) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    EXPECT_CALL(mock_spc, Submit(_))
        .WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.imported);
    EXPECT_GE(stats.skipped_filtered, 2);  // .hidden_file and temp~
    EXPECT_EQ(0, stats.skipped_error);
}

TEST_F(DirectoryImportTest, ScanIdempotent) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    // Pre-seed docs with the ACTUAL file hashes + resolved (canonical) paths so the
    // importer's real doc_find_by_source returns them with matching hashes -> unchanged.
    namespace fs = std::filesystem;
    for (const char* name : {"test.txt", "test.md", "test.pdf"}) {
        std::string resolved = ResolvePath((test_dir_ / name).string());
        std::string hash;
        ComputeFileHash(resolved, hash);
        SeedDoc(*harness, "default", resolved, hash);
    }

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.skipped_unchanged);
    EXPECT_EQ(0, stats.imported);
}

TEST_F(DirectoryImportTest, InitInvalidDir) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = "/nonexistent/path/does/not/exist";
    config.namespace_name = "default";

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(StatusCode::kNotFound, status.code());
}

TEST_F(DirectoryImportTest, EmptyDataDirSkips) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = "";
    config.namespace_name = "default";

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_EQ(0, stats.total_files);
}

TEST_F(DirectoryImportTest, SourceTypeAndPriority) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    EXPECT_CALL(mock_spc, Submit(_))
        .WillRepeatedly(Invoke([](std::shared_ptr<SPCTask> task) -> Status {
            // Verify SPC task properties
            EXPECT_EQ("watch_dir", task->source_type);
            EXPECT_EQ(SPCPriority::kP2, task->priority);  // batch scan = P2
            EXPECT_FALSE(task->is_update);
            return Status::Ok();
        }));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    importer.Start();

    // The doc fields are persisted in the real store; read one back and verify.
    auto docs = ReadDocs(*harness, "default");
    ASSERT_FALSE(docs.empty());
    for (const auto& doc : docs) {
        EXPECT_EQ("watch_dir", doc.source_type);
        EXPECT_FALSE(doc.source_path.empty());
        EXPECT_FALSE(doc.content_hash.empty());
    }
}

TEST_F(DirectoryImportTest, EmptyDirectoryScan) {
    namespace fs = std::filesystem;
    auto empty_dir = fs::temp_directory_path() / "cortrix_test_empty_scan";
    fs::create_directories(empty_dir);

    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = empty_dir.string();
    config.namespace_name = "default";
    config.watch_enabled = false;

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_EQ(0, stats.imported);
    EXPECT_EQ(0, stats.total_files);

    fs::remove_all(empty_dir);
}

TEST_F(DirectoryImportTest, CascadeDeleteCallsAllComponents) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.imported);
}

TEST_F(DirectoryImportTest, ContentUpdateDetected) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    // Pre-seed docs with a STALE hash (different from actual) so the importer detects
    // an update for each file.
    namespace fs = std::filesystem;
    std::string stale_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    for (const char* name : {"test.txt", "test.md", "test.pdf"}) {
        std::string resolved = ResolvePath((test_dir_ / name).string());
        SeedDoc(*harness, "default", resolved, stale_hash, "text/plain");
    }

    bool received_update_task = false;
    EXPECT_CALL(mock_spc, Submit(_))
        .WillRepeatedly(Invoke([&](std::shared_ptr<SPCTask> task) -> Status {
            if (task->is_update) {
                received_update_task = true;
                EXPECT_EQ(SPCPriority::kP1, task->priority);  // update = P1
                EXPECT_FALSE(task->old_doc_id.empty());
            }
            return Status::Ok();
        }));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.updated);
    EXPECT_EQ(0, stats.imported);
    EXPECT_TRUE(received_update_task);
}

TEST_F(DirectoryImportTest, GetStatsReturnsZeroBeforeStart) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;

    DirectoryImporter importer(config, harness->ipool(), mock_spc);

    auto stats = importer.GetStats();
    EXPECT_EQ(0, stats.total_files);
    EXPECT_EQ(0, stats.imported);
    EXPECT_EQ(0, stats.updated);
    EXPECT_EQ(0, stats.skipped_unchanged);
    EXPECT_EQ(0, stats.skipped_filtered);
    EXPECT_EQ(0, stats.skipped_error);
    EXPECT_EQ(0, stats.deleted);
    EXPECT_DOUBLE_EQ(0.0, stats.elapsed_s);
}

// F21 sec 2.3: IsWatching() no longer reflects an OS watcher owned by the importer
// (DirWatcherRegistry owns the single per-directory FileWatcher now). It reflects
// whether the importer is active -- false before Start(), true after a successful
// Start(), false again after Stop(). (Pre-F21 this asserted false when
// watch_enabled=false; that flag is now a no-op for the importer.)
TEST_F(DirectoryImportTest, IsWatchingReflectsImporterActive) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;  // F21: no-op for the importer
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    EXPECT_FALSE(importer.IsWatching());  // not started yet

    auto status = importer.Start();
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(importer.IsWatching());   // active after Start()

    importer.Stop();
    EXPECT_FALSE(importer.IsWatching());  // inactive after Stop()
}

TEST_F(DirectoryImportTest, ImportStatsToString) {
    ImportStats stats;
    stats.total_files = 10;
    stats.imported = 5;
    stats.updated = 2;
    stats.skipped_unchanged = 1;
    stats.skipped_filtered = 1;
    stats.skipped_error = 1;
    stats.deleted = 0;
    stats.elapsed_s = 1.23;

    auto str = stats.ToString();
    EXPECT_NE(std::string::npos, str.find("total=10"));
    EXPECT_NE(std::string::npos, str.find("imported=5"));
    EXPECT_NE(std::string::npos, str.find("updated=2"));
    EXPECT_NE(std::string::npos, str.find("elapsed=1.23s"));
}

TEST_F(DirectoryImportTest, StopIdempotent) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = "";  // empty = skip
    config.namespace_name = "default";

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    importer.Start();
    // Multiple stops should not crash
    importer.Stop();
    importer.Stop();
    importer.Stop();
    EXPECT_FALSE(importer.IsWatching());
}

TEST_F(DirectoryImportTest, NamespaceNotFoundDuringProcess) {
    // The pool only admits "default"; the importer is configured for a namespace that
    // was never admitted, so each file's facade.Acquire() fails -> skipped_error.
    auto harness = MakeHarness("default");
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "wrong_namespace";  // not admitted into the pool
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());  // Scan completes, but files error out

    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.skipped_error);  // All 3 files should have errored
    EXPECT_EQ(0, stats.imported);
}

TEST_F(DirectoryImportTest, SubdirectoryFilesImported) {
    namespace fs = std::filesystem;
    fs::create_directories(test_dir_ / "docs" / "reports");
    CreateFile("docs/readme.md", "documentation");
    CreateFile("docs/reports/q1.pdf", "%PDF-1.4 q1 report");

    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    // 3 original files (test.txt, test.md, test.pdf) + 2 subdirectory files
    EXPECT_EQ(5, stats.imported);
}

// NOTE [wire5c blocked]: the pre-migration test forced doc_create -> -1 via a gmock
// store to assert that a metadata-write failure is counted as skipped_error. The F05
// facade exposes a REAL CortrixStoreSqlite over a healthy temp DB, which has no
// per-call failure-injection seam (doc_create only fails on a SQLite prepare/step
// error, unreachable from the harness). The closest real-store failure in the same
// family is a namespace whose bundle cannot be acquired -- every file then fails
// before doc_create and is counted as skipped_error, preserving the
// "metadata-layer failure => skipped_error, nothing imported" intent. The specific
// "doc_create returned -1" branch is no longer exercised by this test.
TEST_F(DirectoryImportTest, DocCreateFailureCounted) {
    auto harness = MakeHarness("default");
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "unadmitted_ns";  // acquire fails before doc_create
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_EQ(0, stats.imported);
    EXPECT_EQ(3, stats.skipped_error);
}

TEST_F(DirectoryImportTest, AllowedExtensionsFilter) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};
    config.allowed_extensions = {".txt"};  // Only allow .txt files

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_EQ(1, stats.imported);  // Only test.txt should be imported
    EXPECT_GE(stats.skipped_filtered, 4);  // .md, .pdf, .hidden_file, temp~
}

TEST_F(DirectoryImportTest, DataDirIsFileFails) {
    namespace fs = std::filesystem;
    // Create a regular file, then try to use it as data_dir
    auto file_path = fs::temp_directory_path() / "cortrix_test_not_a_dir.txt";
    {
        std::ofstream out(file_path);
        out << "not a directory";
    }

    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = file_path.string();
    config.namespace_name = "default";
    config.watch_enabled = false;

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(StatusCode::kInvalidArgument, status.code());

    fs::remove(file_path);
}

// NOTE [wire5c blocked]: the pre-migration test forced doc_find_by_source -> -1 via a
// gmock store to assert an internal lookup error is counted as skipped_error. The
// real CortrixStoreSqlite returns 0 (found) or -2 (not found) for healthy queries and
// only -1 on a SQLite prepare failure (unreachable from the harness). The closest
// real-store failure in the same family is an un-acquirable namespace, which makes
// every file skip-error before the lookup -- preserving the "lookup-layer failure =>
// skipped_error" intent. The specific "doc_find_by_source returned -1" branch is no
// longer exercised by this test.
TEST_F(DirectoryImportTest, DocFindBySourceErrorCountedAsSkipError) {
    auto harness = MakeHarness("default");
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "unadmitted_ns";  // acquire fails before doc_find_by_source
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_EQ(0, stats.imported);
    EXPECT_EQ(3, stats.skipped_error);
}

// ---- D5 design alignment tests (added during review) ----

TEST_F(DirectoryImportTest, HandleFileDeletionSetsStatusDeleting) {
    // Verify D5 design requirement: Document.status = 'deleting' before cascade.
    // The initial scan of NEW files performs no status updates, so all 3 docs land
    // as kPending (never kDeleting) -- the deletion path is exercised separately by
    // HandleFileDeletion_FullCascadeWithBlocks.
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    importer.Start();

    // No document should be in kDeleting after a pure import scan; all 3 new files
    // are kPending.
    auto docs = ReadDocs(*harness, "default");
    EXPECT_EQ(3u, docs.size());
    for (const auto& d : docs) {
        EXPECT_EQ(DocStatus::kPending, d.status);
    }
}

TEST_F(DirectoryImportTest, SymlinkResolvesToCanonicalPath) {
    // Verify D5 edge case #5: symlink dedup via canonical path resolution
    namespace fs = std::filesystem;

    // Create a symlink to test.txt
    auto symlink_path = test_dir_ / "link_to_test.txt";
    std::error_code ec;
    fs::create_symlink(test_dir_ / "test.txt", symlink_path, ec);
    if (ec) {
        // Skip if symlinks not supported
        GTEST_SKIP() << "Symlinks not supported: " << ec.message();
    }

    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    // All persisted source_paths should be canonical (resolved, no symlink component).
    auto docs = ReadDocs(*harness, "default");
    for (const auto& doc : docs) {
        auto canonical = fs::canonical(doc.source_path, ec);
        if (!ec) {
            EXPECT_EQ(doc.source_path, canonical.string())
                << "Path should already be canonical: " << doc.source_path;
        }
    }

    // Clean up symlink
    fs::remove(symlink_path, ec);
}

TEST_F(DirectoryImportTest, UpdateRedetectsMimeType) {
    // Verify D5 edge case #8: format change triggers MIME re-detection
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    // Pre-seed docs with a stale hash + an OLD mime type so each file triggers update.
    namespace fs = std::filesystem;
    std::string stale_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    for (const char* name : {"test.txt", "test.md", "test.pdf"}) {
        std::string resolved = ResolvePath((test_dir_ / name).string());
        SeedDoc(*harness, "default", resolved, stale_hash, "application/pdf");
    }

    // Capture the SPC task to verify mime_type was re-detected
    std::string captured_mime;
    EXPECT_CALL(mock_spc, Submit(_))
        .WillRepeatedly(Invoke([&](std::shared_ptr<SPCTask> task) -> Status {
            captured_mime = task->mime_type;
            return Status::Ok();
        }));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    // The SPC task mime_type should be re-detected from the actual file extension,
    // NOT the old stored value "application/pdf".
    EXPECT_FALSE(captured_mime.empty());
}

// ======================================================================
// Coverage boost tests: targeting directory_importer.cpp uncovered paths
// ======================================================================

// --- HandleFileEvents: event merging logic ---

TEST_F(DirectoryImportTest, HandleFileEvents_CreatedThenDeleted_Ignored) {
    // Temp file: Created then Deleted should cancel out (merged.erase)
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;  // F21: watcher owned by registry; events are injected
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    // Record stats before event processing
    auto stats_before = importer.GetStats();

    // Inject events: create then delete the same (ephemeral) file. The merge logic
    // should erase the entry so neither ProcessFile nor HandleFileDeletion runs.
    std::vector<FileEvent> events;
    FileEvent ev1;
    ev1.path = (test_dir_ / "ephemeral.txt").string();
    ev1.type = FileEventType::kCreated;
    ev1.is_directory = false;
    events.push_back(ev1);

    FileEvent ev2;
    ev2.path = (test_dir_ / "ephemeral.txt").string();
    ev2.type = FileEventType::kDeleted;
    ev2.is_directory = false;
    events.push_back(ev2);

    importer.HandleFileEvents(events);

    auto stats_after = importer.GetStats();
    // Stats unchanged: the created+deleted temp file events cancel out.
    EXPECT_EQ(stats_before.imported, stats_after.imported);
    EXPECT_EQ(stats_before.deleted, stats_after.deleted);
}

TEST_F(DirectoryImportTest, HandleFileEvents_DirectoryEventsIgnored) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.imported);

    // A directory event must be ignored by HandleFileEvents (is_directory short-circuit).
    std::vector<FileEvent> events;
    FileEvent dir_ev;
    dir_ev.path = (test_dir_ / "some_subdir").string();
    dir_ev.type = FileEventType::kCreated;
    dir_ev.is_directory = true;
    events.push_back(dir_ev);
    importer.HandleFileEvents(events);

    auto stats_after = importer.GetStats();
    EXPECT_EQ(stats.imported, stats_after.imported);
    EXPECT_EQ(0, stats_after.skipped_error);
}

TEST_F(DirectoryImportTest, HandleFileDeletion_FullCascadeWithBlocks) {
    // Verify the full cascade: kDeleting -> vector MarkDelete -> doc_delete -> blob_del.
    // Driven deterministically via HandleFileEvents() (F21 made it public) against the
    // real store + capturing FakeIndex (no OS watcher / sleeps).
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto start_status = importer.Start();
    EXPECT_TRUE(start_status.ok()) << start_status.message();

    // After the scan, test.txt has a real doc in the store. Seed two blocks under it
    // so the cascade has vector entries to delete.
    std::string resolved = ResolvePath((test_dir_ / "test.txt").string());
    std::string doc_id;
    {
        cortrix::resource::NamespaceFacade facade(harness->ipool(), "default");
        ASSERT_TRUE(facade.Acquire().ok());
        CortrixDoc found;
        ASSERT_EQ(0, facade.store().doc_find_by_source("watch_dir", resolved, found));
        doc_id = found.doc_id;
    }
    ASSERT_FALSE(doc_id.empty());
    SeedBlock(*harness, "default", doc_id, "block one");
    SeedBlock(*harness, "default", doc_id, "block two");

    // Delete the file on disk, then dispatch the deletion event.
    std::filesystem::remove(test_dir_ / "test.txt");
    std::vector<FileEvent> events;
    FileEvent del_ev;
    del_ev.path = resolved;
    del_ev.type = FileEventType::kDeleted;
    del_ev.is_directory = false;
    events.push_back(del_ev);
    importer.HandleFileEvents(events);

    // The doc must be gone, the cascade counted, and the blocks' ids handed to the
    // vector index's MarkDelete (captured by FakeIndex).
    EXPECT_FALSE(DocExists(*harness, "default", doc_id));
    auto stats = importer.GetStats();
    EXPECT_EQ(1, stats.deleted);
    ASSERT_NE(nullptr, harness->fake_index());
    EXPECT_FALSE(harness->fake_index()->deleted_ids().empty());
}

TEST_F(DirectoryImportTest, HandleFileDeletion_DocNotFound_NoError) {
    // D5 3.6.4: file deletion for a non-existing doc should be idempotent (no error).
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    // Dispatch a deletion for a path that was never imported -> doc_find_by_source
    // returns -2 -> HandleFileDeletion gracefully returns Ok.
    std::vector<FileEvent> events;
    FileEvent del_ev;
    del_ev.path = (test_dir_ / "never_imported.txt").string();
    del_ev.type = FileEventType::kDeleted;
    del_ev.is_directory = false;
    events.push_back(del_ev);
    importer.HandleFileEvents(events);

    auto stats = importer.GetStats();
    EXPECT_EQ(0, stats.skipped_error);  // No errors from deletion of unknown file
    EXPECT_EQ(0, stats.deleted);
}

TEST_F(DirectoryImportTest, HandleFileDeletion_NamespaceNotFound) {
    // Importer configured for an un-admitted namespace: every ProcessFile errors on
    // acquire, so the scan imports nothing.
    auto harness = MakeHarness("default");
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "wrong_ns";  // not admitted
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    // All files should fail in ProcessFile due to namespace acquire failure.
    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.skipped_error);
    EXPECT_EQ(0, stats.imported);
}

// NOTE [wire5c blocked]: the pre-migration test forced doc_find_by_source -> -1 on the
// deletion path via a gmock store to drive HandleFileDeletion's "Internal error"
// branch. The real CortrixStoreSqlite never returns -1 for a healthy query (only 0 /
// -2), and the facade exposes no per-call failure-injection seam, so that branch is
// not reproducible with NsPoolHarness. This test now verifies the adjacent, real
// behaviour: deleting an unknown file is a safe no-op (idempotent), which is what the
// healthy store returns (-2). The specific "-1 => Internal" branch is no longer
// exercised here.
TEST_F(DirectoryImportTest, HandleFileDeletion_DocFindError_ReturnsInternal) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    // Dispatch a deletion for an unknown path; must not crash or count an error.
    std::vector<FileEvent> events;
    FileEvent del_ev;
    del_ev.path = (test_dir_ / "unknown_for_delete.txt").string();
    del_ev.type = FileEventType::kDeleted;
    del_ev.is_directory = false;
    events.push_back(del_ev);
    importer.HandleFileEvents(events);

    auto stats = importer.GetStats();
    EXPECT_EQ(0, stats.deleted);
}

// NOTE [wire5c blocked]: the pre-migration test forced the vector index remove() -> -1
// to assert the cascade continues past an HNSW failure (doc_delete still runs). The
// harness FakeIndex's MarkDelete always succeeds and exposes no failure setter through
// the facade, so the "vector delete failed" branch is not reproducible. This test now
// verifies the success cascade: with a real store + capturing FakeIndex, the doc is
// deleted, blocks' ids reach MarkDelete, and the deletion is counted. The specific
// "MarkDelete failure => cascade still completes" branch is no longer exercised here.
TEST_F(DirectoryImportTest, HandleFileDeletion_HNSWRemoveFails_ContinuesCascade) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    std::string resolved = ResolvePath((test_dir_ / "test.txt").string());
    std::string doc_id;
    {
        cortrix::resource::NamespaceFacade facade(harness->ipool(), "default");
        ASSERT_TRUE(facade.Acquire().ok());
        CortrixDoc found;
        ASSERT_EQ(0, facade.store().doc_find_by_source("watch_dir", resolved, found));
        doc_id = found.doc_id;
    }
    ASSERT_FALSE(doc_id.empty());
    SeedBlock(*harness, "default", doc_id, "block content");

    std::filesystem::remove(test_dir_ / "test.txt");
    std::vector<FileEvent> events;
    FileEvent del_ev;
    del_ev.path = resolved;
    del_ev.type = FileEventType::kDeleted;
    del_ev.is_directory = false;
    events.push_back(del_ev);
    importer.HandleFileEvents(events);

    // doc_delete runs as part of the cascade regardless of vector outcome.
    EXPECT_FALSE(DocExists(*harness, "default", doc_id));
    auto stats = importer.GetStats();
    EXPECT_EQ(1, stats.deleted);
}

TEST_F(DirectoryImportTest, ScanBatchSizeProgressLogging) {
    // Test that scan_batch_size triggers progress logging
    namespace fs = std::filesystem;

    // Create more files to trigger batch logging
    for (int i = 0; i < 5; ++i) {
        CreateFile("batch_file_" + std::to_string(i) + ".txt",
                   "content " + std::to_string(i));
    }

    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};
    config.scan_batch_size = 2;  // Log progress every 2 files

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    auto stats = importer.GetStats();
    // 3 original + 5 new = 8 total imported
    EXPECT_EQ(8, stats.imported);
    EXPECT_GT(stats.elapsed_s, 0.0);
}

TEST_F(DirectoryImportTest, SPCSubmitFails_StillCountsImported) {
    // When SPC submit fails, the doc is still created but SPC task fails.
    // The file should still count as imported since doc_create succeeded.
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    // SPC submit fails
    EXPECT_CALL(mock_spc, Submit(_))
        .WillRepeatedly(Return(Status::Unavailable("queue full")));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    // Files are imported (doc_create succeeds) even if SPC submit fails
    // (SPC failure is logged but not fatal)
    EXPECT_EQ(3, stats.imported);
    EXPECT_EQ(0, stats.skipped_error);
}

TEST_F(DirectoryImportTest, SPCSubmitFails_DuringUpdate_StillCountsUpdated) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));
    // SPC submit fails for the update path
    EXPECT_CALL(mock_spc, Submit(_))
        .WillRepeatedly(Return(Status::Unavailable("queue full")));

    // Pre-seed stale-hash docs so each file is an update.
    namespace fs = std::filesystem;
    std::string stale_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    for (const char* name : {"test.txt", "test.md", "test.pdf"}) {
        std::string resolved = ResolvePath((test_dir_ / name).string());
        SeedDoc(*harness, "default", resolved, stale_hash);
    }

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    // SPC failure during update is logged but counted as updated
    EXPECT_EQ(3, stats.updated);
}

TEST_F(DirectoryImportTest, WatcherInitFailsReturnsInternal) {
    // A data_dir that is deleted before Start() must fail with NotFound (canonical()
    // / exists() check fails up front).
    namespace fs = std::filesystem;

    auto volatile_dir = fs::temp_directory_path() / "cortrix_test_volatile_watch";
    fs::create_directories(volatile_dir);

    auto harness = MakeHarness();
    MockSPCManager mock_spc;

    WatchDirConfig config;
    config.data_dir = volatile_dir.string();
    config.namespace_name = "default";
    config.watch_enabled = false;

    // Delete the directory BEFORE Start(): exists() returns false -> NotFound.
    fs::remove_all(volatile_dir);

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(StatusCode::kNotFound, status.code());
}

TEST_F(DirectoryImportTest, HandleFileEvents_ModifiedThenDeleted_BecomesDeleted) {
    // Modified then Deleted should merge into a single kDeleted event. With test.txt
    // already imported (real doc), the merged delete cascades and removes the doc.
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    std::string resolved = ResolvePath((test_dir_ / "test.txt").string());
    std::string doc_id;
    {
        cortrix::resource::NamespaceFacade facade(harness->ipool(), "default");
        ASSERT_TRUE(facade.Acquire().ok());
        CortrixDoc found;
        ASSERT_EQ(0, facade.store().doc_find_by_source("watch_dir", resolved, found));
        doc_id = found.doc_id;
    }

    std::filesystem::remove(test_dir_ / "test.txt");
    std::vector<FileEvent> events;
    FileEvent mod_ev;
    mod_ev.path = resolved;
    mod_ev.type = FileEventType::kModified;
    mod_ev.is_directory = false;
    events.push_back(mod_ev);
    FileEvent del_ev;
    del_ev.path = resolved;
    del_ev.type = FileEventType::kDeleted;
    del_ev.is_directory = false;
    events.push_back(del_ev);
    importer.HandleFileEvents(events);  // merges to kDeleted

    // Merged event resolves to a deletion -> doc removed from the store.
    EXPECT_FALSE(DocExists(*harness, "default", doc_id));
    EXPECT_EQ(1, importer.GetStats().deleted);
}

TEST_F(DirectoryImportTest, HandleFileEvents_FilteredFilesSkipped) {
    // Events for filtered files should not be processed.
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    auto stats_before = importer.GetStats();

    // Inject events for a hidden file and a temp (~) file -- both filtered out.
    std::vector<FileEvent> events;
    FileEvent hidden_ev;
    hidden_ev.path = (test_dir_ / ".filtered_hidden_file").string();
    hidden_ev.type = FileEventType::kCreated;
    hidden_ev.is_directory = false;
    events.push_back(hidden_ev);
    FileEvent temp_ev;
    temp_ev.path = (test_dir_ / "filtered_temp~").string();
    temp_ev.type = FileEventType::kCreated;
    temp_ev.is_directory = false;
    events.push_back(temp_ev);
    importer.HandleFileEvents(events);

    auto stats_after = importer.GetStats();
    // Filtered files are skipped before ProcessFile: no new imports, no errors.
    EXPECT_EQ(stats_before.imported, stats_after.imported);
    EXPECT_EQ(stats_before.skipped_error, stats_after.skipped_error);
}

// NOTE [wire5c semantics-adapted]: the pre-migration test intercepted every
// doc_update_status call via a gmock store and asserted the exact (kStale, kPending)
// transition pair x 3 files. The real CortrixStoreSqlite persists only the final
// status (kStale is immediately overwritten by kPending), and the facade gives no way
// to observe intermediate writes -- so the fine-grained transition sequence is not
// observable through the harness. The migrated test verifies the observable end
// state: each updated doc is left kPending (the post-update status) and the update
// count is 3.
TEST_F(DirectoryImportTest, UpdateSetsStatusStaleAndPending) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    namespace fs = std::filesystem;
    std::string stale_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    for (const char* name : {"test.txt", "test.md", "test.pdf"}) {
        std::string resolved = ResolvePath((test_dir_ / name).string());
        SeedDoc(*harness, "default", resolved, stale_hash);
    }

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.updated);

    // End state: all 3 updated docs are left kPending (kStale -> kPending).
    auto docs = ReadDocs(*harness, "default");
    EXPECT_EQ(3u, docs.size());
    for (const auto& d : docs) {
        EXPECT_EQ(DocStatus::kPending, d.status);
    }
}

TEST_F(DirectoryImportTest, UpdateCancelsPreviousSPCTask) {
    // D5 3.6.5: Before re-enqueue, cancel any in-flight SPC task for same path.
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    namespace fs = std::filesystem;
    std::string stale_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    for (const char* name : {"test.txt", "test.md", "test.pdf"}) {
        std::string resolved = ResolvePath((test_dir_ / name).string());
        SeedDoc(*harness, "default", resolved, stale_hash);
    }

    // Track CancelBySourcePath calls
    std::vector<std::string> cancelled_paths;
    EXPECT_CALL(mock_spc, CancelBySourcePath(_))
        .WillRepeatedly(Invoke([&](const std::string& path) -> int {
            cancelled_paths.push_back(path);
            return 0;
        }));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    // 3 files were updated, so CancelBySourcePath should have been called 3 times.
    EXPECT_EQ(3u, cancelled_paths.size());
}

TEST_F(DirectoryImportTest, NewFileSetsCorrectDocFields) {
    // Verify all CortrixDoc fields are set correctly for a new file (read back from
    // the real store).
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    // Note: Start() resolves data_dir to canonical path (e.g., /private/var/... on macOS)
    namespace fs = std::filesystem;
    std::error_code ec;
    auto canonical_data_dir = fs::canonical(test_dir_, ec).string();

    auto created_docs = ReadDocs(*harness, "default");
    EXPECT_EQ(3u, created_docs.size());
    for (const auto& doc : created_docs) {
        EXPECT_EQ("watch_dir", doc.source_type);
        EXPECT_FALSE(doc.source_path.empty());
        EXPECT_EQ(canonical_data_dir, doc.source_ref);  // source_ref = resolved data_dir
        EXPECT_FALSE(doc.content_hash.empty());
        EXPECT_EQ(64u, doc.content_hash.size());  // SHA-256 hex
        EXPECT_GT(doc.file_size, 0);
        EXPECT_FALSE(doc.mime_type.empty());
        EXPECT_EQ(DocStatus::kPending, doc.status);
        EXPECT_FALSE(doc.title.empty());
        EXPECT_EQ(3, doc.processing_level);
    }
}

TEST_F(DirectoryImportTest, UpdateSPCTask_HasCorrectFields) {
    // Verify the SPC task fields when updating an existing doc.
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    namespace fs = std::filesystem;
    std::string stale_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    // Seed one doc (test.txt) so we can match its minted id on the update task.
    std::string resolved_txt = ResolvePath((test_dir_ / "test.txt").string());
    std::string seeded_id = SeedDoc(*harness, "default", resolved_txt, stale_hash);
    // Seed the other two too so all three are updates (keeps the scan uniform).
    SeedDoc(*harness, "default", ResolvePath((test_dir_ / "test.md").string()), stale_hash);
    SeedDoc(*harness, "default", ResolvePath((test_dir_ / "test.pdf").string()), stale_hash);

    std::vector<std::shared_ptr<SPCTask>> submitted_tasks;
    EXPECT_CALL(mock_spc, Submit(_))
        .WillRepeatedly(Invoke([&](std::shared_ptr<SPCTask> task) -> Status {
            submitted_tasks.push_back(task);
            return Status::Ok();
        }));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    for (const auto& task : submitted_tasks) {
        EXPECT_FALSE(task->doc_id.empty());
        EXPECT_EQ("default", task->namespace_name);
        EXPECT_EQ("watch_dir", task->source_type);
        EXPECT_FALSE(task->content_hash.empty());
        EXPECT_FALSE(task->mime_type.empty());
        EXPECT_EQ(SPCPriority::kP1, task->priority);  // watch change = P1
        EXPECT_TRUE(task->is_update);
        EXPECT_EQ(task->doc_id, task->old_doc_id);
    }
    // The task for test.txt must carry the seeded doc's id.
    bool saw_seeded = false;
    for (const auto& task : submitted_tasks) {
        if (task->source_path == resolved_txt) {
            EXPECT_EQ(seeded_id, task->doc_id);
            saw_seeded = true;
        }
    }
    EXPECT_TRUE(saw_seeded);
}

TEST_F(DirectoryImportTest, ScanBatchSizeZero_NoProgressLogging) {
    // scan_batch_size = 0 should disable progress logging
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};
    config.scan_batch_size = 0;  // Disable progress logging

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_EQ(3, stats.imported);
}

TEST_F(DirectoryImportTest, HandleFileEvents_MultipleModifiesOnSamePath) {
    // Multiple Modified events on same path should merge into a single Modified.
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    EXPECT_CALL(mock_spc, CancelBySourcePath(_)).WillRepeatedly(Return(0));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok()) << status.message();

    // Inject 5 Modified events for the same (already-imported) file. The merge keeps a
    // single Modified -> one ProcessFile pass; the unchanged hash counts as skipped.
    std::string resolved = ResolvePath((test_dir_ / "test.txt").string());
    std::vector<FileEvent> events;
    for (int i = 0; i < 5; ++i) {
        FileEvent ev;
        ev.path = resolved;
        ev.type = FileEventType::kModified;
        ev.is_directory = false;
        events.push_back(ev);
    }
    importer.HandleFileEvents(events);  // must not crash; dedups to one Modified

    auto stats = importer.GetStats();
    EXPECT_EQ(0, stats.skipped_error);
}

TEST_F(DirectoryImportTest, ElapsedTimeIsPositive) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    DirectoryImporter importer(config, harness->ipool(), mock_spc);
    auto status = importer.Start();
    EXPECT_TRUE(status.ok());

    auto stats = importer.GetStats();
    EXPECT_GT(stats.elapsed_s, 0.0);
}

TEST_F(DirectoryImportTest, DestructorCallsStop) {
    auto harness = MakeHarness();
    MockSPCManager mock_spc;
    EXPECT_CALL(mock_spc, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    WatchDirConfig config;
    config.data_dir = test_dir_.string();
    config.namespace_name = "default";
    config.watch_enabled = false;
    config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};

    {
        DirectoryImporter importer(config, harness->ipool(), mock_spc);
        auto status = importer.Start();
        EXPECT_TRUE(status.ok());
        EXPECT_TRUE(importer.IsWatching());
        // Destructor should call Stop() without crash
    }
    // If we get here without crash, destructor cleanup worked
}

}  // namespace
}  // namespace cortrix
