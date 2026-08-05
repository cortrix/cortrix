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
    auto before = mgr_.LiveTaskInputs();
    ASSERT_TRUE(before.ok());
    ASSERT_EQ(before.value().size(), 1u);
    const std::string c_input = before.value()[0].second;

    ASSERT_EQ(SubmitOne("tenant-d", "id2", "CONTENT_D").status, 200);

    EXPECT_EQ(mgr_.CountAll().value(), 2);
    // tenant-c's task still points at tenant-c's own content.
    auto live = mgr_.LiveTaskInputs();
    ASSERT_TRUE(live.ok());
    bool c_intact = false;
    for (const auto& [tid, fp] : live.value()) {
        (void)tid;
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
    auto live = mgr_.LiveTaskInputs();
    ASSERT_TRUE(live.ok() && live.value().size() == 1u);
    std::ifstream in(live.value()[0].second, std::ios::binary);
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
    auto live = mgr_.LiveTaskInputs();
    ASSERT_TRUE(live.ok() && live.value().size() == 1u);
    const std::string input = live.value()[0].second;
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

// The live set is what makes the reaper safe; pin its semantics directly,
// including that a terminal task stops counting as a reference.
TEST_F(BatchIngestFx, LiveTaskInputsIgnoresTerminalTasks) {
    const std::string path = (fs::path(dir_) / "counted.txt").string();
    async::TaskInfo t;
    t.namespace_id = "ns"; t.doc_id = "d"; t.filename = "f"; t.filepath = path;
    t.task_type = async::kTaskDocParse; t.status = async::task_status::kQueued;
    const auto created = mgr_.CreateTask(std::move(t)).value();

    auto live = mgr_.LiveTaskInputs();
    ASSERT_TRUE(live.ok());
    ASSERT_EQ(live.value().size(), 1u);
    EXPECT_EQ(live.value()[0].first, created.task_id);
    EXPECT_EQ(live.value()[0].second, path);

    ASSERT_TRUE(mgr_.MarkCancelled(created.task_id).ok());
    auto after = mgr_.LiveTaskInputs();
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(after.value().empty());
}

// --- transitions that must not destroy in-flight work -----------------------

// A refresh rewrites the matched row in place. If that row is already being
// processed, the worker holds the old TaskInfo: releasing its input strands the
// parse, and the worker's later MarkCompleted lands on a row that now represents
// the NEW submission — reporting content as processed that was never read. An
// in-flight task must keep both its row and its input.
TEST_F(BatchIngestFx, ResubmitDoesNotRewriteARowAWorkerIsUsing) {
    ASSERT_EQ(SubmitOne("ns", "doc", "V1").status, 200);
    auto claimed = sched_->Dequeue(1);
    ASSERT_TRUE(claimed.ok() && claimed.value().has_value());
    const async::TaskInfo running = *claimed.value();
    ASSERT_EQ(running.status, async::task_status::kProcessing);
    ASSERT_TRUE(fs::exists(running.filepath));

    // New content for the same doc while the first one is mid-flight.
    ASSERT_EQ(SubmitOne("ns", "doc", "V2").status, 200);

    // The running task is untouched: same row, still processing, input still there.
    auto row = mgr_.GetTask(running.task_id);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().status, async::task_status::kProcessing);
    EXPECT_EQ(row.value().filepath, running.filepath);
    EXPECT_TRUE(fs::exists(running.filepath))
        << "the input of a task a worker is processing was released";
    // The resubmission became its own task rather than hijacking that row.
    EXPECT_EQ(mgr_.CountAll().value(), 2);

    // And the worker finishing does not mark the new submission complete.
    async::TaskFinalizer fin(&mgr_, dir_);
    fin.Complete(running, "doc-v1", std::chrono::steady_clock::now());
    auto tasks = mgr_.ListByNamespace("ns", 10, 0);
    ASSERT_TRUE(tasks.ok());
    int still_queued = 0;
    for (const auto& t : tasks.value()) {
        if (t.status == async::task_status::kQueued) ++still_queued;
    }
    EXPECT_EQ(still_queued, 1) << "the new submission was reported complete unprocessed";
}

// Dequeue selects a queued row and only then marks it processing. A cancel can
// land in that gap, moving the row to cancelled and releasing its input; an
// unconditional mark would resurrect it and dispatch a worker with no input.
//
// Reproduced at the two steps Dequeue performs, in order, with the cancel
// interleaved between them — driving Dequeue() itself cannot express the gap
// because it holds its lock across both.
TEST_F(BatchIngestFx, CancelInsideTheDequeueGapCannotResurrectTheRow) {
    ASSERT_EQ(SubmitOne("ns", "doc", "payload").status, 200);

    // Step 1 of Dequeue: pick a row that is still queued.
    auto picked = mgr_.SelectOldestQueuedTaskExcluding({});
    ASSERT_TRUE(picked.ok() && picked.value().has_value());
    const async::TaskInfo selected = *picked.value();
    ASSERT_EQ(selected.status, async::task_status::kQueued);
    ASSERT_TRUE(fs::exists(selected.filepath));

    // The gap: a cancel lands, taking the row terminal and releasing its input.
    async::DocumentTaskHandler handler(sched_.get(), &mgr_, nullptr, nullptr, dir_);
    ASSERT_EQ(handler.CancelTask(selected.task_id).status, 200);
    ASSERT_FALSE(fs::exists(selected.filepath));

    // Step 2 of Dequeue must now refuse: the row is no longer queued.
    const Status marked = mgr_.MarkProcessing(selected.task_id, /*worker_id=*/1);
    EXPECT_FALSE(marked.ok())
        << "a cancelled row was marked processing, dispatching a worker with no input";

    auto row = mgr_.GetTask(selected.task_id);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().status, async::task_status::kCancelled)
        << "a cancelled row was resurrected to processing";
}

// The scheduler surfaces that conflict as "nothing to dispatch" rather than an
// error, so the worker loop simply tries again.
TEST_F(BatchIngestFx, DequeueReportsNoWorkWhenTheRowStoppedBeingQueued) {
    ASSERT_EQ(SubmitOne("ns", "doc", "payload").status, 200);
    auto tasks = mgr_.ListByNamespace("ns", 10, 0);
    ASSERT_TRUE(tasks.ok() && tasks.value().size() == 1u);

    async::DocumentTaskHandler handler(sched_.get(), &mgr_, nullptr, nullptr, dir_);
    ASSERT_EQ(handler.CancelTask(tasks.value()[0].task_id).status, 200);

    auto picked = sched_->Dequeue(1);
    ASSERT_TRUE(picked.ok()) << "a lost race must not surface as a scheduler error";
    EXPECT_FALSE(picked.value().has_value());
}

// The status read before the refresh is stale by the time the write happens:
// CancelTask does not share the scheduler mutex. Reproduced by cancelling between
// the lookup and the refresh — the write must lose, not resurrect the row.
TEST_F(BatchIngestFx, CancelBetweenLookupAndRefreshCannotResurrectTheRow) {
    ASSERT_EQ(SubmitOne("ns", "doc", "V1").status, 200);
    auto tasks = mgr_.ListByNamespace("ns", 10, 0);
    ASSERT_TRUE(tasks.ok() && tasks.value().size() == 1u);
    const async::TaskInfo original = tasks.value()[0];

    // What Enqueue does first: find the debounce candidate while it is queued.
    auto found = mgr_.FindRecentTaskByDocId("ns", "doc", async::kTaskDocParse, 3600);
    ASSERT_TRUE(found.ok() && found.value().has_value());
    ASSERT_EQ(found.value()->status, async::task_status::kQueued);

    // The gap: a cancel takes that row terminal and releases its input.
    async::DocumentTaskHandler handler(sched_.get(), &mgr_, nullptr, nullptr, dir_);
    ASSERT_EQ(handler.CancelTask(original.task_id).status, 200);

    // What Enqueue does next. The write is the authority and must refuse.
    async::SubmitRequest refresh;
    refresh.namespace_id = "ns"; refresh.doc_id = "doc";
    refresh.content_hash = "different"; refresh.filepath = "/tmp/whatever";
    refresh.task_type = async::kTaskDocParse;
    EXPECT_FALSE(mgr_.UpdateTaskForDebounce(original.task_id, refresh).ok())
        << "a cancelled row was reset to queued for a different submission";

    auto row = mgr_.GetTask(original.task_id);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().status, async::task_status::kCancelled)
        << "the cancel response reported terminal but the row was resurrected";
}

// End to end: after a cancel, a resubmission with NEW content must become its own
// live task rather than reviving the cancelled one.
TEST_F(BatchIngestFx, ResubmitAfterCancelGetsItsOwnTask) {
    ASSERT_EQ(SubmitOne("ns", "doc", "V1").status, 200);
    auto tasks = mgr_.ListByNamespace("ns", 10, 0);
    ASSERT_TRUE(tasks.ok() && tasks.value().size() == 1u);
    const std::string cancelled_id = tasks.value()[0].task_id;

    async::DocumentTaskHandler handler(sched_.get(), &mgr_, nullptr, nullptr, dir_);
    ASSERT_EQ(handler.CancelTask(cancelled_id).status, 200);

    ASSERT_EQ(SubmitOne("ns", "doc", "V2").status, 200);

    EXPECT_EQ(mgr_.CountAll().value(), 2) << "the resubmission revived the cancelled row";
    auto old_row = mgr_.GetTask(cancelled_id);
    ASSERT_TRUE(old_row.ok());
    EXPECT_EQ(old_row.value().status, async::task_status::kCancelled);
    // The new submission is live and owns its own input.
    auto live = mgr_.LiveTaskInputs();
    ASSERT_TRUE(live.ok());
    ASSERT_EQ(live.value().size(), 1u);
    EXPECT_NE(live.value()[0].first, cancelled_id);
    EXPECT_TRUE(fs::exists(live.value()[0].second));
}

// The same-content branch has the same problem without any race: a cancelled task
// returned as the debounce result swallows the resubmission — the caller is handed
// a terminal task_id and the new input is released, so nothing ever processes it.
TEST_F(BatchIngestFx, SameContentResubmitAfterCancelIsNotSwallowed) {
    ASSERT_EQ(SubmitOne("ns", "doc", "SAME").status, 200);
    auto tasks = mgr_.ListByNamespace("ns", 10, 0);
    ASSERT_TRUE(tasks.ok() && tasks.value().size() == 1u);
    const std::string cancelled_id = tasks.value()[0].task_id;

    async::DocumentTaskHandler handler(sched_.get(), &mgr_, nullptr, nullptr, dir_);
    ASSERT_EQ(handler.CancelTask(cancelled_id).status, 200);
    ASSERT_EQ(FilesInDir(), 0u);  // cancel released its input

    // Identical content again, still inside the debounce window.
    ASSERT_EQ(SubmitOne("ns", "doc", "SAME").status, 200);

    EXPECT_EQ(mgr_.CountAll().value(), 2)
        << "the resubmission was merged into a cancelled task and will never be processed";
    auto live = mgr_.LiveTaskInputs();
    ASSERT_TRUE(live.ok());
    ASSERT_EQ(live.value().size(), 1u) << "no live task carries the resubmission";
    EXPECT_NE(live.value()[0].first, cancelled_id);
    EXPECT_TRUE(fs::exists(live.value()[0].second))
        << "the resubmission's input was released with no live task to read it";
}

// Task rows store whatever string was handed to F42, so the same file can be
// spelled differently by two tasks. The reference check has to compare
// filesystem identity, not the stored strings, or finishing one task deletes a
// file the other still needs.
TEST_F(BatchIngestFx, ReferenceCheckMatchesOnFilesystemIdentityNotStringForm) {
    const fs::path real = fs::path(dir_) / "shared.txt";
    std::ofstream(real) << "payload";
    // Two live tasks naming the SAME file, spelled differently.
    const std::string plain = real.string();
    const std::string dotted = (fs::path(dir_) / "." / "shared.txt").string();
    ASSERT_NE(plain, dotted);

    auto make = [&](const std::string& doc, const std::string& fp) {
        async::TaskInfo t;
        t.namespace_id = "ns"; t.doc_id = doc; t.filename = "f"; t.filepath = fp;
        t.task_type = async::kTaskDocParse; t.status = async::task_status::kQueued;
        return mgr_.CreateTask(std::move(t)).value();
    };
    const async::TaskInfo t1 = make("d1", plain);
    make("d2", dotted);

    async::TaskFinalizer fin(&mgr_, dir_);
    fin.Complete(t1, "doc-1", std::chrono::steady_clock::now());
    EXPECT_TRUE(fs::exists(real))
        << "a live task's input was deleted because its row spelled the path differently";
}

}  // namespace
