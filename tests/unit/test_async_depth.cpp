// DEPTH anti-regression tests for src/async/ TaskManager -- targets state-transition
// edges, scheduler-support queries, cleanup boundaries, and the CX_ERR_* identity carried
// on Status failures that test_task_manager.cpp does not already pin down.
//
// Deterministic: in-memory SQLite TaskManager, explicit Unix timestamps (no sleeps,
// no wall-clock dependence for the cleanup-window math).
//
// Suite/fixture names are globally unique (AsyncDepth* prefix) per gtest's global-string
// suite-name rule.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/async/f42_error.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_type.h"
#include "cortrix/common/status.h"

namespace cortrix::async {
namespace {

TaskInfo AsyncDepthMakeTask(const std::string& ns, const std::string& doc_id,
                            const std::string& hash = "h1") {
    TaskInfo t;
    t.namespace_id = ns;
    t.filename = "doc.pdf";
    t.filepath = "/tmp/doc.pdf";
    t.doc_id = doc_id;
    t.content_hash = hash;
    t.total_pages = 100;
    return t;
}

// True iff `s` carries the given CX_ERR_* token (F42Status prefixes "CX_ERR_X" / "...: detail").
bool CarriesCode(const cortrix::Status& s, const char* token) {
    return s.message().rfind(token, 0) == 0;
}

class AsyncDepthTaskFx : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(mgr_.Init(":memory:").ok()); }
    TaskManager mgr_;
};

// ---- Full happy-path lifecycle: queued -> processing -> completed ----

TEST_F(AsyncDepthTaskFx, FullLifecycleQueuedProcessingCompleted) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "doc1"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;

    ASSERT_TRUE(mgr_.MarkProcessing(id, /*worker_id=*/7).ok());
    auto p = mgr_.GetTask(id);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().status, task_status::kProcessing);
    EXPECT_EQ(p.value().worker_id, 7);
    EXPECT_FALSE(p.value().started_at.empty());

    ASSERT_TRUE(mgr_.MarkCompleted(id, "final-doc-id").ok());
    auto done = mgr_.GetTask(id);
    ASSERT_TRUE(done.ok());
    EXPECT_EQ(done.value().status, task_status::kCompleted);
    EXPECT_EQ(done.value().doc_id, "final-doc-id");
    EXPECT_FLOAT_EQ(done.value().progress_pct, 100.0f);
    EXPECT_FALSE(done.value().completed_at.empty());
}

// ---- CountAll ----

TEST_F(AsyncDepthTaskFx, CountAllReflectsInserts) {
    auto c0 = mgr_.CountAll();
    ASSERT_TRUE(c0.ok());
    EXPECT_EQ(c0.value(), 0);
    ASSERT_TRUE(mgr_.CreateTask(AsyncDepthMakeTask("ns", "a")).ok());
    ASSERT_TRUE(mgr_.CreateTask(AsyncDepthMakeTask("ns", "b")).ok());
    auto c2 = mgr_.CountAll();
    ASSERT_TRUE(c2.ok());
    EXPECT_EQ(c2.value(), 2);
}

// ---- ListByNamespace pagination + scoping ----

TEST_F(AsyncDepthTaskFx, ListByNamespacePaginatesNewestFirst) {
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(mgr_.CreateTask(AsyncDepthMakeTask("nsP", "d" + std::to_string(i))).ok());
    }
    ASSERT_TRUE(mgr_.CreateTask(AsyncDepthMakeTask("other", "x")).ok());

    auto page1 = mgr_.ListByNamespace("nsP", /*limit=*/2, /*offset=*/0);
    ASSERT_TRUE(page1.ok());
    EXPECT_EQ(page1.value().size(), 2u);

    auto page2 = mgr_.ListByNamespace("nsP", /*limit=*/2, /*offset=*/2);
    ASSERT_TRUE(page2.ok());
    EXPECT_EQ(page2.value().size(), 2u);

    // Offset past the end -> empty page, still Ok.
    auto past = mgr_.ListByNamespace("nsP", /*limit=*/10, /*offset=*/100);
    ASSERT_TRUE(past.ok());
    EXPECT_TRUE(past.value().empty());

    // Unknown namespace -> empty.
    auto none = mgr_.ListByNamespace("ghost", 10, 0);
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());
}

// ---- State transitions on missing tasks carry CX_ERR_TASK_NOT_FOUND ----

TEST_F(AsyncDepthTaskFx, MarkProcessingMissingCarriesTaskNotFoundToken) {
    auto st = mgr_.MarkProcessing("ghost", 1);
    EXPECT_EQ(st.code(), StatusCode::kNotFound);
    EXPECT_TRUE(CarriesCode(st, "CX_ERR_TASK_NOT_FOUND"));
}

TEST_F(AsyncDepthTaskFx, MarkCancelledMissingIsNotFound) {
    auto st = mgr_.MarkCancelled("ghost");
    EXPECT_EQ(st.code(), StatusCode::kNotFound);
}

// ---- RequestCancel terminal repeat -> AlreadyExists + CX_ERR_TASK_CANCELLING ----

TEST_F(AsyncDepthTaskFx, RequestCancelOnCompletedTaskReturnsCancellingIdentity) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "doneTask"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(id, "dd").ok());

    TaskInfo out;
    auto st = mgr_.RequestCancel(id, &out);
    EXPECT_EQ(st.code(), StatusCode::kAlreadyExists);
    EXPECT_TRUE(CarriesCode(st, "CX_ERR_TASK_CANCELLING"));
}

TEST_F(AsyncDepthTaskFx, RequestCancelProcessingMovesToCancellingThenMarkCancelled) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "cxl"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 2).ok());

    TaskInfo out;
    ASSERT_TRUE(mgr_.RequestCancel(id, &out).ok());
    EXPECT_EQ(out.status, task_status::kCancelling);
    EXPECT_TRUE(out.cancel_requested);

    ASSERT_TRUE(mgr_.MarkCancelled(id).ok());
    auto got = mgr_.GetTask(id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kCancelled);
    EXPECT_FALSE(got.value().completed_at.empty());
}

// ---- MarkFailed persists GEN-Agent error fields ----

TEST_F(AsyncDepthTaskFx, MarkFailedPersistsCodeMsgAndStructuredData) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "ferr"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 3).ok());

    ASSERT_TRUE(mgr_.MarkFailed(id, "CX_ERR_PARSE_FAILED", "bad pdf",
                                R"({"page":4})").ok());
    auto got = mgr_.GetTask(id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kFailed);
    EXPECT_EQ(got.value().error_code, "CX_ERR_PARSE_FAILED");
    EXPECT_EQ(got.value().error_msg, "bad pdf");
    EXPECT_EQ(got.value().structured_data, R"({"page":4})");
}

// ---- UpdateTaskForDebounce on missing task -> CX_ERR_TASK_NOT_FOUND ----

TEST_F(AsyncDepthTaskFx, UpdateTaskForDebounceMissingIsNotFound) {
    SubmitRequest req;
    req.namespace_id = "ns";
    req.doc_id = "d";
    req.content_hash = "newhash";
    req.filepath = "/tmp/new.pdf";
    auto r = mgr_.UpdateTaskForDebounce("ghost", req);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
}

TEST_F(AsyncDepthTaskFx, UpdateTaskForDebounceResetsToQueuedWithNewHash) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "dbg", "oldhash"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;

    SubmitRequest req;
    req.namespace_id = "ns";
    req.doc_id = "dbg";
    req.content_hash = "newhash";
    req.filepath = "/tmp/refreshed.pdf";
    auto r = mgr_.UpdateTaskForDebounce(id, req);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().status, task_status::kQueued);
    EXPECT_EQ(r.value().content_hash, "newhash");
    EXPECT_EQ(r.value().filepath, "/tmp/refreshed.pdf");
    EXPECT_FLOAT_EQ(r.value().progress_pct, 0.0f);
}

TEST_F(AsyncDepthTaskFx, UpdateTaskForDebounceRefusesARowThatIsNoLongerQueued) {
    // This case previously asserted the opposite: it marked the row processing and
    // required the refresh to succeed. That rewrites a row a worker is already
    // using — the worker loses its input and its later MarkCompleted lands on a row
    // that now represents a different submission. The write is the authority, so a
    // row that left `queued` (dequeued, or cancelled between lookup and refresh)
    // must refuse and let the caller mint a separate task.
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "dbg2", "oldhash"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 4).ok());

    SubmitRequest req;
    req.namespace_id = "ns";
    req.doc_id = "dbg2";
    req.content_hash = "newhash";
    req.filepath = "/tmp/refreshed.pdf";
    auto r = mgr_.UpdateTaskForDebounce(id, req);
    EXPECT_FALSE(r.ok()) << "a row a worker is using was reset to queued";
    EXPECT_TRUE(CarriesCode(r.status(), "CX_ERR_DOC_PROCESSING_IN_PROGRESS"));

    auto got = mgr_.GetTask(id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kProcessing);
    EXPECT_EQ(got.value().content_hash, "oldhash");
}

// ---- FindRecentTaskByDocId window + task_type scoping ----

TEST_F(AsyncDepthTaskFx, FindRecentTaskScopesByTaskTypeAndWindow) {
    TaskInfo parse = AsyncDepthMakeTask("ns", "shared");
    parse.task_type = kTaskDocParse;
    ASSERT_TRUE(mgr_.CreateTask(parse).ok());

    // Same doc, a recent parse task is found within a wide window.
    auto found = mgr_.FindRecentTaskByDocId("ns", "shared", kTaskDocParse, /*window_seconds=*/3600);
    ASSERT_TRUE(found.ok());
    EXPECT_TRUE(found.value().has_value());

    // A different task_type for the same doc should not match the parse row.
    auto other = mgr_.FindRecentTaskByDocId("ns", "shared", kTaskDocParse + 99, 3600);
    ASSERT_TRUE(other.ok());
    EXPECT_FALSE(other.value().has_value());

    // Unknown doc -> none.
    auto miss = mgr_.FindRecentTaskByDocId("ns", "nope", kTaskDocParse, 3600);
    ASSERT_TRUE(miss.ok());
    EXPECT_FALSE(miss.value().has_value());
}

// ---- SelectOldestQueuedTaskExcluding: per-doc_id mutex + queue exhaustion ----

TEST_F(AsyncDepthTaskFx, SelectOldestQueuedExcludesActiveDocIds) {
    ASSERT_TRUE(mgr_.CreateTask(AsyncDepthMakeTask("ns", "docX")).ok());
    ASSERT_TRUE(mgr_.CreateTask(AsyncDepthMakeTask("ns", "docY")).ok());

    // No active docs -> oldest queued returned.
    auto pick = mgr_.SelectOldestQueuedTaskExcluding({});
    ASSERT_TRUE(pick.ok());
    ASSERT_TRUE(pick.value().has_value());

    // Exclude both queued docs -> nothing selectable.
    auto none = mgr_.SelectOldestQueuedTaskExcluding({{"ns", "docX"}, {"ns", "docY"}});
    ASSERT_TRUE(none.ok());
    EXPECT_FALSE(none.value().has_value());
}

TEST_F(AsyncDepthTaskFx, SelectOldestQueuedEmptyTableReturnsNone) {
    auto pick = mgr_.SelectOldestQueuedTaskExcluding({});
    ASSERT_TRUE(pick.ok());
    EXPECT_FALSE(pick.value().has_value());
}

// ---- Cleanup boundaries (deterministic timestamps) ----

TEST_F(AsyncDepthTaskFx, DeleteExpiredKeepsRowsWithinRetention) {
    // A completed task whose updated_at is "now" must survive a retention sweep.
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "fresh"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(id, "d").ok());

    // now_unix far in the past so (now - retention) is even earlier -> nothing expired.
    auto del = mgr_.DeleteExpired(/*now_unix=*/0, /*retention_days=*/7);
    ASSERT_TRUE(del.ok());
    EXPECT_EQ(del.value(), 0);
    EXPECT_EQ(mgr_.CountAll().value(), 1);
}

TEST_F(AsyncDepthTaskFx, SweepZombiesLeavesFreshProcessingUntouched) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "z"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());

    // now_unix=0 so the (now - zombie_hours*3600) threshold is negative -> nothing stale.
    auto swept = mgr_.SweepZombies(/*now_unix=*/0, /*zombie_hours=*/24);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 0);
    auto got = mgr_.GetTask(id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kProcessing);
}

TEST_F(AsyncDepthTaskFx, CleanupSweepsOnEmptyTableReturnZero) {
    EXPECT_EQ(mgr_.DeleteExpired(0, 7).value(), 0);
    EXPECT_EQ(mgr_.SweepZombies(0, 24).value(), 0);
    EXPECT_EQ(mgr_.SweepTimedOut(0, 1800).value(), 0);
    EXPECT_EQ(mgr_.RequeueStaleProcessing(0, 24).value(), 0);
}

// ---- Operations on an uninitialized manager fail gracefully (no crash) ----

TEST(AsyncDepthUninitTest, GetOnUninitializedManagerIsStorageFailure) {
    TaskManager mgr;  // never Init'd
    auto r = mgr.GetTask("anything");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

// ---- Init is idempotent ----

TEST(AsyncDepthInitTest, InitTwiceIsIdempotent) {
    TaskManager mgr;
    ASSERT_TRUE(mgr.Init(":memory:").ok());
    EXPECT_TRUE(mgr.Init(":memory:").ok());
}

// ---- SweepTimedOut flips long-running processing rows + stamps CX_ERR_TASK_TIMEOUT ----

TEST_F(AsyncDepthTaskFx, SweepTimedOutMarksFailedWithTimeoutCode) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "slow"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());  // started_at stamped ~now

    // now far in the future so (now - timeout) is still after the row's started_at ->
    // the row is past its budget.
    int64_t far_future = 4102444800;  // 2100-01-01
    auto swept = mgr_.SweepTimedOut(far_future, /*timeout_seconds=*/1800);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 1);
    auto got = mgr_.GetTask(id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kFailed);
    EXPECT_EQ(got.value().error_code, "CX_ERR_TASK_TIMEOUT");
}

TEST_F(AsyncDepthTaskFx, SweepTimedOutIgnoresRowsWithoutStartedAt) {
    // A still-queued task has no started_at -> never times out.
    ASSERT_TRUE(mgr_.CreateTask(AsyncDepthMakeTask("ns", "queued")).ok());
    auto swept = mgr_.SweepTimedOut(4102444800, 1);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 0);
}

// ---- RequeueStaleProcessing returns fresh processing rows to the queue ----

TEST_F(AsyncDepthTaskFx, RequeueStaleProcessingResetsFreshProcessingToQueued) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "recover"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 9).ok());  // updated_at ~now

    // RequeueStaleProcessing re-queues rows YOUNGER than the zombie threshold
    // (updated_at >= now - zombie_hours). Using the real wall clock, the just-stamped
    // row's updated_at is well inside a 24h window -> matched and re-queued.
    int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    auto n = mgr_.RequeueStaleProcessing(now_unix, /*zombie_hours=*/24);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 1);
    auto got = mgr_.GetTask(id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kQueued);
    EXPECT_EQ(got.value().worker_id, -1);  // worker cleared
}

// ---- MarkProcessing has no source-state guard: it is a raw UPDATE by id ----

TEST_F(AsyncDepthTaskFx, MarkProcessingRefusesARowThatIsNoLongerQueued) {
    // This previously asserted the opposite ("no state-machine guard ... changes>0
    // -> Ok"), which is the defect: Dequeue selects a queued row and only then
    // marks it, so an unconditional update resurrects a row that was cancelled or
    // claimed in between and dispatches a worker for it.
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "raw"));
    ASSERT_TRUE(created.ok());
    const std::string id = created.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(id, "d").ok());

    auto st = mgr_.MarkProcessing(id, 2);
    EXPECT_FALSE(st.ok()) << "a terminal row was marked processing again";
    // A live-but-not-queued row is a transient conflict, not "no such task".
    EXPECT_TRUE(CarriesCode(st, "CX_ERR_DOC_PROCESSING_IN_PROGRESS"));
    auto got = mgr_.GetTask(id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kCompleted);
    EXPECT_EQ(got.value().worker_id, 1);
}

// ---- CreateTask: empty id is auto-generated; explicit id is honored ----

TEST_F(AsyncDepthTaskFx, CreateTaskAutoGeneratesUlidWhenEmpty) {
    TaskInfo t = AsyncDepthMakeTask("ns", "doc");
    t.task_id.clear();
    auto r = mgr_.CreateTask(t);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().task_id.empty());
    EXPECT_FALSE(r.value().created_at.empty());
    EXPECT_FALSE(r.value().updated_at.empty());
}

TEST_F(AsyncDepthTaskFx, CreateTaskHonorsClientProvidedId) {
    TaskInfo t = AsyncDepthMakeTask("ns", "doc");
    t.task_id = "client-supplied-id";
    auto r = mgr_.CreateTask(t);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().task_id, "client-supplied-id");
    EXPECT_TRUE(mgr_.GetTask("client-supplied-id").ok());
}

// ---- UpdateProgress persists per-page counters ----

TEST_F(AsyncDepthTaskFx, UpdateProgressPersistsPageCountersAndPhase) {
    auto created = mgr_.CreateTask(AsyncDepthMakeTask("ns", "prog"));
    ASSERT_TRUE(created.ok());
    TaskInfo t = created.value();
    t.processed_pages = 30;
    t.total_pages = 100;
    t.failed_pages = {3, 7};
    t.progress_pct = 30.0f;
    t.current_phase = task_phase::kEnriching;
    t.worker_id = 5;
    ASSERT_TRUE(mgr_.UpdateProgress(t).ok());

    auto got = mgr_.GetTask(t.task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().processed_pages, 30);
    EXPECT_EQ(got.value().total_pages, 100);
    ASSERT_EQ(got.value().failed_pages.size(), 2u);
    EXPECT_EQ(got.value().failed_pages[0], 3);
    EXPECT_EQ(got.value().failed_pages[1], 7);
    EXPECT_EQ(got.value().current_phase, task_phase::kEnriching);
    EXPECT_EQ(got.value().worker_id, 5);
}

}  // namespace
}  // namespace cortrix::async
