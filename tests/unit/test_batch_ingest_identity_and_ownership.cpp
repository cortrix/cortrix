// Batch ingest identity and input ownership, exercised through the REAL
// production path: BatchSubmitService → F42TaskSubmitterAdapter → TaskScheduler
// → TaskManager.
//
// A capturing mock cannot see any of this. The scheduler is where debounce
// identity is decided and where a materialized input silently stops having an
// owner, so a test that stops at the submit seam reports green while two
// namespaces are sharing one task and files are accumulating.
//
// Covered:
//   - identity is (namespace_id, doc_id, task_type), not doc_id alone;
//   - every materialized input has exactly one owner at all times — merge,
//     refresh and submit-failure all hand back the file that lost its owner;
//   - an input is released only after a durably recorded terminal state, and
//     only when no other live task still references it;
//   - the queued-cancel path, which never reaches TaskFinalizer, still releases.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cortrix/async/document_task_handler.h"
#include "cortrix/async/managed_input.h"
#include "cortrix/async/task_finalizer.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/server/batch_temp_store.h"
#include "cortrix/server/f42_task_submitter_adapter.h"

namespace fs = std::filesystem;
using namespace cortrix;

namespace {

class BatchIngestFx : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("cortrix_batch_ingest_" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_);
        dir_ = server::BatchTempDir((root_ / "data").string());
        fs::create_directories(dir_);
        ASSERT_TRUE(mgr_.Init((root_ / "tasks.db").string()).ok());

        sched_ = std::make_unique<async::TaskScheduler>(&mgr_, nullptr);
        // Same wiring bootstrap installs: the scheduler hands back an input it
        // decided not to adopt, the service hands back one whose submit failed.
        async::TaskManager* tm = &mgr_;
        const std::string d = dir_;
        sched_->SetUnadoptedInputReleaser(
            [d, tm](const std::string& p, const std::string& owner) {
                async::ReleaseManagedPath(d, p, owner, tm);
            });
        adapter_ = std::make_unique<server::F42TaskSubmitterAdapter>(sched_.get(), nullptr);
        svc_ = std::make_unique<server::BatchSubmitService>(adapter_.get());
        svc_->SetMaterializeDir(dir_);
        svc_->SetInputReleaser([d, tm](const std::string& p) {
            async::ReleaseManagedPath(d, p, /*exclude=*/"", tm);
        });
    }
    void TearDown() override { fs::remove_all(root_); }

    server::BatchHttpResult SubmitOne(const std::string& ns, const std::string& doc_id,
                                      const std::string& content) {
        server::BatchRequest req;
        req.namespace_id = ns;
        req.documents.push_back({doc_id, content, "", "doc.txt"});
        return svc_->Submit(req);
    }

    size_t FilesInDir() const {
        size_t n = 0;
        for (const auto& e : fs::directory_iterator(dir_)) {
            if (e.is_regular_file()) ++n;
        }
        return n;
    }

    fs::path root_;
    std::string dir_;
    async::TaskManager mgr_;
    std::unique_ptr<async::TaskScheduler> sched_;
    std::unique_ptr<server::F42TaskSubmitterAdapter> adapter_;
    std::unique_ptr<server::BatchSubmitService> svc_;
};

// --- identity ---------------------------------------------------------------

// doc_id is caller-chosen and therefore only unique within a namespace. Keyed on
// doc_id alone, the debounce merged two namespaces into one task: the second
// namespace's document was never ingested even though the API reported success.
TEST_F(BatchIngestFx, SameDocIdInTwoNamespacesCreatesTwoTasks) {
    ASSERT_EQ(SubmitOne("tenant-a", "shared-id", "CONTENT_A").status, 200);
    ASSERT_EQ(SubmitOne("tenant-b", "shared-id", "CONTENT_A").status, 200);

    EXPECT_EQ(mgr_.CountAll().value(), 2)
        << "one namespace's submission was merged into another's task";
    EXPECT_EQ(FilesInDir(), 2u) << "each live task must keep its own input";
}

// The worse half of the same defect: with different content the refresh path
// repointed the FIRST namespace's task at the second namespace's file while its
// namespace_id stayed put, so that content would be parsed into the wrong
// namespace.
TEST_F(BatchIngestFx, RefreshNeverRepointsAnotherNamespacesTask) {
    ASSERT_EQ(SubmitOne("tenant-c", "id2", "CONTENT_C").status, 200);
    auto before = mgr_.ActiveFilepaths();
    ASSERT_TRUE(before.ok());
    ASSERT_EQ(before.value().size(), 1u);
    const std::string c_input = before.value()[0];

    ASSERT_EQ(SubmitOne("tenant-d", "id2", "CONTENT_D").status, 200);

    EXPECT_EQ(mgr_.CountAll().value(), 2);
    // tenant-c's task still points at tenant-c's own content.
    auto live = mgr_.ActiveFilepaths();
    ASSERT_TRUE(live.ok());
    bool c_intact = false;
    for (const std::string& fp : live.value()) {
        if (fp != c_input) continue;
        std::ifstream in(fp, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        c_intact = (body == "CONTENT_C");
    }
    EXPECT_TRUE(c_intact) << "a namespace's task input was replaced by another's";
}

// Reservations are per (namespace, doc) too: one namespace's in-flight document
// must not block another namespace's document that merely shares an id.
TEST_F(BatchIngestFx, DequeueReservationIsNamespaceScoped) {
    ASSERT_EQ(SubmitOne("tenant-a", "same-id", "A").status, 200);
    ASSERT_EQ(SubmitOne("tenant-b", "same-id", "B").status, 200);

    auto first = sched_->Dequeue(1);
    ASSERT_TRUE(first.ok() && first.value().has_value());
    auto second = sched_->Dequeue(2);
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(second.value().has_value())
        << "the second namespace's task was blocked by the first namespace's doc_id";
    EXPECT_NE(first.value()->namespace_id, second.value()->namespace_id);
}

// --- ownership of materialized inputs ---------------------------------------

// An exact retry inside the debounce window merges into the existing task, which
// keeps its own input — so the file just written for the retry has no owner and
// must be handed back immediately, not left for a sweep.
TEST_F(BatchIngestFx, DebounceMergeReleasesTheUnadoptedInput) {
    ASSERT_EQ(SubmitOne("ns", "doc", "SAME").status, 200);
    ASSERT_EQ(FilesInDir(), 1u);

    ASSERT_EQ(SubmitOne("ns", "doc", "SAME").status, 200);  // merged, no new task

    EXPECT_EQ(mgr_.CountAll().value(), 1);
    EXPECT_EQ(FilesInDir(), 1u) << "the merged retry left an input nobody owns";
}

// A refresh repoints the task at the new input, which orphans the previous one at
// a moment only the scheduler can see.
TEST_F(BatchIngestFx, DebounceRefreshReleasesTheSupersededInput) {
    ASSERT_EQ(SubmitOne("ns", "doc", "V1").status, 200);
    ASSERT_EQ(FilesInDir(), 1u);

    ASSERT_EQ(SubmitOne("ns", "doc", "V2").status, 200);  // same doc, new content

    EXPECT_EQ(mgr_.CountAll().value(), 1);
    ASSERT_EQ(FilesInDir(), 1u) << "the superseded input was left behind";
    // The surviving file is the new one.
    auto live = mgr_.ActiveFilepaths();
    ASSERT_TRUE(live.ok() && live.value().size() == 1u);
    std::ifstream in(live.value()[0], std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "V2");
}

// --- release safety ---------------------------------------------------------

// Inputs named before the server minted its own filenames could be shared by two
// tasks. Finishing one must not pull the input out from under another that is
// still queued.
TEST_F(BatchIngestFx, ReleaseSkipsAPathAnotherLiveTaskStillReferences) {
    const fs::path shared = fs::path(dir_) / "legacy-shared.txt";
    std::ofstream(shared) << "legacy";

    auto make = [&](const std::string& doc) {
        async::TaskInfo t;
        t.namespace_id = "ns"; t.doc_id = doc; t.filename = "doc.txt";
        t.filepath = shared.string(); t.task_type = async::kTaskDocParse;
        t.status = async::task_status::kQueued;
        return mgr_.CreateTask(std::move(t)).value();
    };
    const async::TaskInfo t1 = make("d1");
    const async::TaskInfo t2 = make("d2");

    async::TaskFinalizer fin(&mgr_, dir_);
    fin.Complete(t1, "doc-1", std::chrono::steady_clock::now());
    EXPECT_TRUE(fs::exists(shared))
        << "an input another live task still references was deleted";

    fin.Complete(t2, "doc-2", std::chrono::steady_clock::now());
    EXPECT_FALSE(fs::exists(shared)) << "the last owner did not release the input";
}

// A rejected terminal write leaves the task live; it will be retried or re-queued
// and needs its input to still be there.
TEST_F(BatchIngestFx, InputSurvivesWhenTheTerminalWriteIsRejected) {
    const fs::path input = fs::path(dir_) / "orphan-row.txt";
    std::ofstream(input) << "payload";

    async::TaskInfo ghost;  // never inserted → Mark* returns NotFound
    ghost.task_id = "no-such-task";
    ghost.namespace_id = "ns";
    ghost.filepath = input.string();
    ghost.task_type = async::kTaskDocParse;

    async::TaskFinalizer fin(&mgr_, dir_);
    fin.Complete(ghost, "doc-1", std::chrono::steady_clock::now());
    EXPECT_TRUE(fs::exists(input))
        << "the input was released even though the terminal state was not recorded";
}

// A queued task cancels terminally inside CancelTask without ever reaching a
// worker, so TaskFinalizer never sees it — that path has to release too.
TEST_F(BatchIngestFx, QueuedCancelReleasesItsInput) {
    ASSERT_EQ(SubmitOne("ns", "doc", "payload").status, 200);
    auto live = mgr_.ActiveFilepaths();
    ASSERT_TRUE(live.ok() && live.value().size() == 1u);
    const std::string input = live.value()[0];
    ASSERT_TRUE(fs::exists(input));

    async::DocumentTaskHandler handler(sched_.get(), &mgr_, nullptr, nullptr, dir_);
    auto tasks = mgr_.ListByNamespace("ns", 10, 0);
    ASSERT_TRUE(tasks.ok() && tasks.value().size() == 1u);
    auto res = handler.CancelTask(tasks.value()[0].task_id);
    ASSERT_EQ(res.status, 200);

    auto after = mgr_.GetTask(tasks.value()[0].task_id);
    ASSERT_TRUE(after.ok());
    ASSERT_EQ(after.value().status, async::task_status::kCancelled);
    EXPECT_FALSE(fs::exists(input)) << "a cancelled queued task retained its input";
}

// The live-reference guard is what makes the reaper safe; pin its semantics
// directly, including that a terminal task does not count as a reference.
TEST_F(BatchIngestFx, LiveReferenceCountIgnoresTerminalTasks) {
    const std::string path = (fs::path(dir_) / "counted.txt").string();
    async::TaskInfo t;
    t.namespace_id = "ns"; t.doc_id = "d"; t.filename = "f"; t.filepath = path;
    t.task_type = async::kTaskDocParse; t.status = async::task_status::kQueued;
    const auto created = mgr_.CreateTask(std::move(t)).value();

    auto n = mgr_.CountOtherLiveTasksWithFilepath(path, "someone-else");
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 1);

    // Excluding the only referencing task leaves nobody.
    auto self = mgr_.CountOtherLiveTasksWithFilepath(path, created.task_id);
    ASSERT_TRUE(self.ok());
    EXPECT_EQ(self.value(), 0);

    // A terminal task is not a reference.
    ASSERT_TRUE(mgr_.MarkCancelled(created.task_id).ok());
    auto done = mgr_.CountOtherLiveTasksWithFilepath(path, "someone-else");
    ASSERT_TRUE(done.ok());
    EXPECT_EQ(done.value(), 0);
}

}  // namespace
