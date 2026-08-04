// Lifecycle of server-materialized batch inputs (server::BatchTempDir).
//
// Those files exist only to give the F42 doc-parse worker something to open.
// Nothing used to delete them: the sole reference is tasks.filepath, and the
// retention cron deletes task rows with plain SQL, so after the retention window
// the files became unreferenced orphans that no sweep could even identify. The
// two reclaim paths under test:
//   1. TaskFinalizer releases the input at every terminal exit;
//   2. SweepOrphanedBatchInputs reclaims what (1) can never see — tasks ended by
//      the bulk zombie/timeout UPDATEs, crash leftovers, and files superseded by
//      the F42 debounce refresh.
// The negative cases matter as much as the positive ones: a queued task's input
// must survive (it is re-read after a restart), and caller-owned paths outside
// the managed dir must never be touched.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "cortrix/async/task_finalizer.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/server/batch_temp_store.h"

namespace fs = std::filesystem;
using namespace cortrix;

namespace {

class BatchTempLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("cortrix_batch_life_" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_);
        dir_ = server::BatchTempDir((root_ / "data").string());
        fs::create_directories(dir_);
        ASSERT_TRUE(mgr_.Init((root_ / "tasks.db").string()).ok());
    }
    void TearDown() override { fs::remove_all(root_); }

    /// Create a materialized-input file and backdate it past the sweep's grace
    /// window so age is never what keeps it alive in these tests.
    std::string MakeInput(const std::string& name, const std::string& body = "x") {
        const fs::path p = fs::path(dir_) / name;
        std::ofstream(p) << body;
        const auto old = fs::file_time_type::clock::now() -
                         std::chrono::seconds(server::kOrphanGraceSeconds * 2);
        std::error_code ec;
        fs::last_write_time(p, old, ec);
        return p.string();
    }

    async::TaskInfo MakeTask(const std::string& filepath, const std::string& status) {
        async::TaskInfo t;
        t.namespace_id = "ns";
        t.filename = "doc.txt";
        t.filepath = filepath;
        t.doc_id = "doc-" + status;
        t.status = status;
        t.task_type = async::kTaskDocParse;
        auto created = mgr_.CreateTask(std::move(t));
        EXPECT_TRUE(created.ok());
        return created.value();
    }

    fs::path root_;
    std::string dir_;
    async::TaskManager mgr_;
};

// Every terminal exit releases the task's managed input.
TEST_F(BatchTempLifecycleTest, FinalizerReleasesInputOnAllTerminalExits) {
    async::TaskFinalizer fin(&mgr_, dir_);
    const auto t0 = std::chrono::steady_clock::now();

    const std::string done = MakeInput("completed.txt");
    fin.Complete(MakeTask(done, async::task_status::kQueued), "doc-1", t0);
    EXPECT_FALSE(fs::exists(done)) << "Complete must release the materialized input";

    const std::string failed = MakeInput("failed.txt");
    fin.Fail(MakeTask(failed, async::task_status::kQueued), "CX_ERR_PARSE_FAILED",
             "boom", nlohmann::json::object(), t0);
    EXPECT_FALSE(fs::exists(failed)) << "Fail must release the materialized input";

    const std::string cancelled = MakeInput("cancelled.txt");
    fin.Cancel(MakeTask(cancelled, async::task_status::kQueued), t0);
    EXPECT_FALSE(fs::exists(cancelled)) << "Cancel must release the materialized input";
}

// Paths the server did not materialize are caller-owned and must survive: watcher
// and connector tasks point at real user files.
TEST_F(BatchTempLifecycleTest, FinalizerNeverTouchesCallerOwnedPaths) {
    const fs::path outside = root_ / "user_document.txt";
    std::ofstream(outside) << "caller owned";

    async::TaskFinalizer fin(&mgr_, dir_);
    fin.Complete(MakeTask(outside.string(), async::task_status::kQueued), "doc-1",
                 std::chrono::steady_clock::now());

    EXPECT_TRUE(fs::exists(outside))
        << "a path outside the managed dir must never be deleted";
}

// With no managed dir configured (standalone/test wiring) nothing is deleted.
TEST_F(BatchTempLifecycleTest, FinalizerIsInertWithoutManagedDir) {
    const std::string kept = MakeInput("kept.txt");
    async::TaskFinalizer fin(&mgr_);  // no managed dir
    fin.Complete(MakeTask(kept, async::task_status::kQueued), "doc-1",
                 std::chrono::steady_clock::now());
    EXPECT_TRUE(fs::exists(kept));
}

// The sweep reclaims exactly the files no live task references — and nothing else.
TEST_F(BatchTempLifecycleTest, SweepReclaimsOrphansAndSparesLiveInputs) {
    const std::string queued     = MakeInput("queued.txt");
    const std::string processing = MakeInput("processing.txt");
    const std::string zombied    = MakeInput("zombied.txt");
    const std::string crashed    = MakeInput("crashed.txt");  // no task row at all

    MakeTask(queued, async::task_status::kQueued);
    MakeTask(processing, async::task_status::kProcessing);
    // A task ended by the bulk zombie/timeout UPDATE: terminal, but no handler
    // ever ran, so the finalizer never saw it.
    MakeTask(zombied, async::task_status::kFailed);

    auto swept = server::SweepOrphanedBatchInputs(dir_, &mgr_);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 2);

    EXPECT_TRUE(fs::exists(queued))
        << "a queued task's input is re-read after restart and must survive";
    EXPECT_TRUE(fs::exists(processing));
    EXPECT_FALSE(fs::exists(zombied)) << "bulk-terminal tasks must be reclaimed";
    EXPECT_FALSE(fs::exists(crashed)) << "unreferenced leftovers must be reclaimed";
}

// A file written moments ago is left alone even if unreferenced, so the sweep is
// safe if it is ever run while submissions are in flight.
TEST_F(BatchTempLifecycleTest, SweepSparesFilesInsideTheGraceWindow) {
    const fs::path fresh = fs::path(dir_) / "just_written.txt";
    std::ofstream(fresh) << "x";  // current mtime, no task row

    auto swept = server::SweepOrphanedBatchInputs(dir_, &mgr_);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 0);
    EXPECT_TRUE(fs::exists(fresh));
}

// A missing directory is success, not an error (fresh deployment, batch unused).
TEST_F(BatchTempLifecycleTest, SweepOnMissingDirIsSuccess) {
    auto swept = server::SweepOrphanedBatchInputs((root_ / "nope").string(), &mgr_);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 0);
}

}  // namespace
