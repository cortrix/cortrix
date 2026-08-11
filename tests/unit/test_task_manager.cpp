#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_type.h"

// S1 coverage: TaskType enum SoT, tasks-table CRUD, the §3.1 status state
// machine, the Issue 2 scheduler-support queries, and the Issue 4 cleanup paths —
// all against an in-memory SQLite TaskManager.
namespace cortrix::async {
namespace {

int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

TaskInfo MakeTask(const std::string& ns, const std::string& doc_id,
                  const std::string& hash = "h1") {
    TaskInfo t;
    t.namespace_id = ns;
    t.filename = "big.pdf";
    t.filepath = "/tmp/big.pdf";
    t.doc_id = doc_id;
    t.content_hash = hash;
    t.total_pages = 500;
    return t;
}

class TaskManagerTest : public ::testing::Test {
  protected:
    void SetUp() override { ASSERT_TRUE(mgr_.Init(":memory:").ok()); }
    TaskManager mgr_;
};

// ---- TaskType enum (SoT, §3.2) --------------------------------------------

TEST(TaskTypeTest, EnumValuesAreStable) {
    EXPECT_EQ(static_cast<int>(kTaskDocParse), 1);
    EXPECT_EQ(static_cast<int>(kTaskWatcherFanout), 2);
    EXPECT_EQ(static_cast<int>(kTaskDocSummary), 3);
}

// ---- Init / CRUD ----------------------------------------------------------

TEST_F(TaskManagerTest, InitIsIdempotent) {
    EXPECT_TRUE(mgr_.Init(":memory:").ok());  // second call no-op
    auto n = mgr_.CountAll();
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0);
}

TEST_F(TaskManagerTest, CreateGeneratesUlidAndTimestamps) {
    auto r = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().task_id.size(), 26u);  // ULID = 26 chars
    EXPECT_FALSE(r.value().created_at.empty());
    EXPECT_EQ(r.value().created_at, r.value().updated_at);
    EXPECT_EQ(r.value().status, std::string(task_status::kQueued));
    EXPECT_EQ(r.value().task_type, static_cast<int>(kTaskDocParse));
}

TEST_F(TaskManagerTest, CreateRespectsClientProvidedId) {
    TaskInfo t = MakeTask("ns1", "doc1");
    t.task_id = "explicit-id";
    auto r = mgr_.CreateTask(t);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().task_id, "explicit-id");
}

TEST_F(TaskManagerTest, GetRoundTripsAllFields) {
    TaskInfo t = MakeTask("ns1", "doc1", "hash-abc");
    t.trace_id = "trace-123";
    t.total_pages = 1500;
    // Caller document metadata must survive the queue so it can reach the doc row and
    // round-trip to query results (the batch path dropped it before this column existed).
    t.metadata_json = R"({"beir_corpus_id":"CID-42","tag":"unit"})";
    auto created = mgr_.CreateTask(t);
    ASSERT_TRUE(created.ok());

    auto got = mgr_.GetTask(created.value().task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().namespace_id, "ns1");
    EXPECT_EQ(got.value().doc_id, "doc1");
    EXPECT_EQ(got.value().content_hash, "hash-abc");
    EXPECT_EQ(got.value().trace_id, "trace-123");
    EXPECT_EQ(got.value().total_pages, 1500);
    EXPECT_EQ(got.value().worker_id, -1);  // unassigned → NULL → -1
    EXPECT_TRUE(got.value().failed_pages.empty());
    EXPECT_EQ(got.value().metadata_json, R"({"beir_corpus_id":"CID-42","tag":"unit"})");
}

TEST_F(TaskManagerTest, GetMissingReturnsTaskNotFound) {
    auto got = mgr_.GetTask("nope");
    ASSERT_FALSE(got.ok());
    EXPECT_EQ(got.status().code(), StatusCode::kNotFound);
    EXPECT_NE(got.status().message().find("CX_ERR_TASK_NOT_FOUND"), std::string::npos);
}

TEST_F(TaskManagerTest, UpdateProgressPersistsPageState) {
    auto created = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(created.ok());
    TaskInfo t = created.value();
    t.processed_pages = 237;
    t.total_pages = 500;
    t.failed_pages = {42, 168};
    t.progress_pct = 47.4f;
    t.eta_seconds = 180;
    t.current_phase = task_phase::kParsing;
    t.worker_id = 2;
    ASSERT_TRUE(mgr_.UpdateProgress(t).ok());

    auto got = mgr_.GetTask(t.task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().processed_pages, 237);
    EXPECT_EQ(got.value().failed_pages, (std::vector<int>{42, 168}));
    EXPECT_FLOAT_EQ(got.value().progress_pct, 47.4f);
    EXPECT_EQ(got.value().eta_seconds, 180);
    EXPECT_EQ(got.value().current_phase, std::string(task_phase::kParsing));
    EXPECT_EQ(got.value().worker_id, 2);
}

TEST_F(TaskManagerTest, UpdateProgressMissingReturnsNotFound) {
    TaskInfo t;
    t.task_id = "ghost";
    auto s = mgr_.UpdateProgress(t);
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(TaskManagerTest, ListByNamespaceNewestFirstAndScoped) {
    mgr_.CreateTask(MakeTask("nsA", "d1"));
    mgr_.CreateTask(MakeTask("nsA", "d2"));
    mgr_.CreateTask(MakeTask("nsB", "d3"));
    auto a = mgr_.ListByNamespace("nsA", 10, 0);
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().size(), 2u);
    for (const auto& t : a.value()) EXPECT_EQ(t.namespace_id, "nsA");
    // pagination
    auto page = mgr_.ListByNamespace("nsA", 1, 0);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page.value().size(), 1u);
}

// ---- State machine --------------------------------------------------------

TEST_F(TaskManagerTest, MarkProcessingSetsWorkerAndStartedAt) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    auto got = mgr_.GetTask(c.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kProcessing));
    EXPECT_EQ(got.value().worker_id, 1);
    EXPECT_FALSE(got.value().started_at.empty());
}

TEST_F(TaskManagerTest, MarkCompletedSetsDocIdAnd100Pct) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(c.value().task_id, "final-doc").ok());
    auto got = mgr_.GetTask(c.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCompleted));
    EXPECT_EQ(got.value().doc_id, "final-doc");
    EXPECT_FLOAT_EQ(got.value().progress_pct, 100.0f);
    EXPECT_FALSE(got.value().completed_at.empty());
}

TEST_F(TaskManagerTest, MarkFailedPersistsGenAgentFields) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.MarkFailed(c.value().task_id, "CX_ERR_PARSE_FAILED",
                                "bad page", R"({"page_number":42})")
                    .ok());
    auto got = mgr_.GetTask(c.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kFailed));
    EXPECT_EQ(got.value().error_code, "CX_ERR_PARSE_FAILED");
    EXPECT_EQ(got.value().error_msg, "bad page");
    EXPECT_NE(got.value().structured_data.find("page_number"), std::string::npos);
}

TEST_F(TaskManagerTest, RequestCancelQueuedGoesStraightToCancelled) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    TaskInfo out;
    ASSERT_TRUE(mgr_.RequestCancel(c.value().task_id, &out).ok());
    EXPECT_EQ(out.status, std::string(task_status::kCancelled));
    EXPECT_TRUE(out.cancel_requested);
    EXPECT_FALSE(out.completed_at.empty());
}

TEST_F(TaskManagerTest, RequestCancelProcessingGoesToCancelling) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    TaskInfo out;
    ASSERT_TRUE(mgr_.RequestCancel(c.value().task_id, &out).ok());
    EXPECT_EQ(out.status, std::string(task_status::kCancelling));
    EXPECT_TRUE(out.cancel_requested);
    EXPECT_TRUE(out.completed_at.empty());  // not terminal yet
}

TEST_F(TaskManagerTest, RepeatCancelOnCancellingReturns423Identity) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.RequestCancel(c.value().task_id, nullptr).ok());
    // second cancel → CX_ERR_TASK_CANCELLING (423 identity)
    auto s = mgr_.RequestCancel(c.value().task_id, nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_TASK_CANCELLING"), std::string::npos);
}

TEST_F(TaskManagerTest, CancelMissingTaskReturnsNotFound) {
    auto s = mgr_.RequestCancel("ghost", nullptr);
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(TaskManagerTest, MarkCancelledTerminalFromCancelling) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.RequestCancel(c.value().task_id, nullptr).ok());
    ASSERT_TRUE(mgr_.MarkCancelled(c.value().task_id).ok());
    auto got = mgr_.GetTask(c.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCancelled));
    EXPECT_FALSE(got.value().completed_at.empty());
}

// ---- Terminal-transition guards (issue #57, same family as the #31
// MarkProcessing guard): terminal rows are immutable, queued cannot jump
// straight to completed/failed, and the late-cancel race edges
// (cancelling -> completed/failed) stay legal so the row never strands.

TEST_F(TaskManagerTest, MarkCancelledRejectedOnCompletedRow) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    const std::string id = c.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(id, "final-doc").ok());
    auto st = mgr_.MarkCancelled(id);
    EXPECT_FALSE(st.ok()) << "a completed row was flipped to cancelled";
    EXPECT_NE(st.message().find("CX_ERR_DOC_PROCESSING_IN_PROGRESS"),
              std::string::npos);
    auto got = mgr_.GetTask(id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCompleted));
    EXPECT_EQ(got.value().doc_id, "final-doc");
}

TEST_F(TaskManagerTest, MarkCompletedRejectedOnCancelledRow) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    const std::string id = c.value().task_id;
    ASSERT_TRUE(mgr_.RequestCancel(id, nullptr).ok());  // queued -> cancelled
    auto st = mgr_.MarkCompleted(id, "late-doc");
    EXPECT_FALSE(st.ok()) << "a cancelled row was resurrected to completed";
    EXPECT_NE(st.message().find("CX_ERR_DOC_PROCESSING_IN_PROGRESS"),
              std::string::npos);
    auto got = mgr_.GetTask(id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCancelled));
    EXPECT_TRUE(got.value().doc_id != "late-doc");
}

TEST_F(TaskManagerTest, MarkFailedRejectedOnCompletedRow) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    const std::string id = c.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(id, "final-doc").ok());
    auto st = mgr_.MarkFailed(id, "CX_ERR_PARSE_FAILED", "late error", "{}");
    EXPECT_FALSE(st.ok()) << "a completed row was flipped to failed";
    auto got = mgr_.GetTask(id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCompleted));
    EXPECT_TRUE(got.value().error_code.empty());
}

TEST_F(TaskManagerTest, MarkCompletedRejectedFromQueued) {
    // completed is only reachable through processing (the dequeue claim).
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    auto st = mgr_.MarkCompleted(c.value().task_id, "shortcut-doc");
    EXPECT_FALSE(st.ok()) << "a queued row skipped processing to completed";
    auto got = mgr_.GetTask(c.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kQueued));
}

TEST_F(TaskManagerTest, MarkCompletedAllowedFromCancellingLateCancelRace) {
    // The cancel request landed after the worker's last checkpoint: the work IS
    // done, the best-effort cancel loses, and the row must not strand in
    // cancelling (F42 §3.1 race-edge note).
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    const std::string id = c.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());
    ASSERT_TRUE(mgr_.RequestCancel(id, nullptr).ok());  // processing -> cancelling
    ASSERT_TRUE(mgr_.MarkCompleted(id, "raced-doc").ok());
    auto got = mgr_.GetTask(id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCompleted));
    EXPECT_EQ(got.value().doc_id, "raced-doc");
}

TEST_F(TaskManagerTest, MarkFailedAllowedFromCancellingLateCancelRace) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    const std::string id = c.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(id, 1).ok());
    ASSERT_TRUE(mgr_.RequestCancel(id, nullptr).ok());
    ASSERT_TRUE(mgr_.MarkFailed(id, "CX_ERR_PARSE_FAILED", "boom", "{}").ok());
    auto got = mgr_.GetTask(id);
    EXPECT_EQ(got.value().status, std::string(task_status::kFailed));
    EXPECT_EQ(got.value().error_code, "CX_ERR_PARSE_FAILED");
}

TEST_F(TaskManagerTest, MarkCancelledAllowedFromQueued) {
    // queued -> cancelled is a legal state-machine edge (never-started task).
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(mgr_.MarkCancelled(c.value().task_id).ok());
    auto got = mgr_.GetTask(c.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCancelled));
}

// (Missing-row NOT_FOUND for all three guarded Marks stays pinned by
// StateTransitionsOnMissingTaskReturnNotFound below.)

// State transitions on a missing task_id all return CX_ERR_TASK_NOT_FOUND (the
// changes==0 branch of each UPDATE).
TEST_F(TaskManagerTest, StateTransitionsOnMissingTaskReturnNotFound) {
    EXPECT_EQ(mgr_.MarkCompleted("ghost", "d").code(), StatusCode::kNotFound);
    EXPECT_EQ(mgr_.MarkFailed("ghost", "CX_ERR_PARSE_FAILED", "x").code(),
              StatusCode::kNotFound);
    EXPECT_EQ(mgr_.MarkCancelled("ghost").code(), StatusCode::kNotFound);
    SubmitRequest req;
    req.content_hash = "h";
    req.filepath = "/tmp/x.pdf";
    EXPECT_EQ(mgr_.UpdateTaskForDebounce("ghost", req).status().code(),
              StatusCode::kNotFound);
}

// ---- State machine: additional branch coverage ------------------------------

// RequestCancel on each terminal status hits the else → 423 TASK_CANCELLING branch
// (the only branch previously exercised was 'cancelling').
TEST_F(TaskManagerTest, RequestCancelOnTerminalStatusesReturns423) {
    // completed
    auto comp = mgr_.CreateTask(MakeTask("ns1", "dc"));
    ASSERT_TRUE(mgr_.MarkProcessing(comp.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(comp.value().task_id, "doc").ok());
    auto s_comp = mgr_.RequestCancel(comp.value().task_id, nullptr);
    EXPECT_FALSE(s_comp.ok());
    EXPECT_NE(s_comp.message().find("CX_ERR_TASK_CANCELLING"), std::string::npos);

    // failed
    auto fail = mgr_.CreateTask(MakeTask("ns1", "df"));
    ASSERT_TRUE(mgr_.MarkProcessing(fail.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.MarkFailed(fail.value().task_id, "CX_ERR_PARSE_FAILED", "x").ok());
    EXPECT_FALSE(mgr_.RequestCancel(fail.value().task_id, nullptr).ok());

    // cancelled (queued → cancelled, then repeat)
    auto canc = mgr_.CreateTask(MakeTask("ns1", "dcx"));
    ASSERT_TRUE(mgr_.RequestCancel(canc.value().task_id, nullptr).ok());  // queued→cancelled
    auto s_canc = mgr_.RequestCancel(canc.value().task_id, nullptr);      // repeat → 423
    EXPECT_FALSE(s_canc.ok());
    EXPECT_NE(s_canc.message().find("CX_ERR_TASK_CANCELLING"), std::string::npos);
}

// RequestCancel with out==nullptr on the queued→cancelled (terminal) path covers
// the 'if (out)' false branch and the terminal-update arm without an out-param.
TEST_F(TaskManagerTest, RequestCancelQueuedWithNullOutStillTransitions) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "dn"));
    ASSERT_TRUE(mgr_.RequestCancel(c.value().task_id, nullptr).ok());
    auto got = mgr_.GetTask(c.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCancelled));
    EXPECT_TRUE(got.value().cancel_requested);
    EXPECT_FALSE(got.value().completed_at.empty());  // terminal arm set completed_at
}

// MarkCompleted with an empty doc_id exercises BindNullableText's NULL arm (the
// success cases above always pass a non-empty doc_id).
TEST_F(TaskManagerTest, MarkCompletedWithEmptyDocIdLeavesDocIdNull) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "")); // doc_id empty from the start
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(c.value().task_id, "").ok());
    auto got = mgr_.GetTask(c.value().task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, std::string(task_status::kCompleted));
    EXPECT_TRUE(got.value().doc_id.empty());  // NULL → "" on read-back
}

// MarkFailed with empty error_code/error_msg/structured_data exercises the NULL
// arm of BindNullableText for all three optional columns.
TEST_F(TaskManagerTest, MarkFailedWithEmptyFieldsBindsNull) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "de"));
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.MarkFailed(c.value().task_id, "", "", "").ok());
    auto got = mgr_.GetTask(c.value().task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, std::string(task_status::kFailed));
    EXPECT_TRUE(got.value().error_code.empty());
    EXPECT_TRUE(got.value().error_msg.empty());
    EXPECT_TRUE(got.value().structured_data.empty());
}

// CreateTask with all optional fields empty + an assigned worker_id covers the
// BindNullableText NULL arms and the worker_id non-null bind arm in one INSERT.
TEST_F(TaskManagerTest, CreateTaskWithEmptyOptionalsAndWorkerId) {
    TaskInfo t;
    t.namespace_id = "ns1";
    t.filename = "f.pdf";
    t.filepath = "/tmp/f.pdf";
    t.worker_id = 7;  // pre-assigned (non-null bind arm at INSERT)
    // doc_id / content_hash / current_phase / trace_id / ... all empty.
    auto r = mgr_.CreateTask(t);
    ASSERT_TRUE(r.ok());
    auto got = mgr_.GetTask(r.value().task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_TRUE(got.value().doc_id.empty());
    EXPECT_TRUE(got.value().content_hash.empty());
    EXPECT_EQ(got.value().worker_id, 7);
}

// UpdateProgress with worker_id < 0 covers the bind_null arm (the persist test
// uses worker_id=2, the non-null arm).
TEST_F(TaskManagerTest, UpdateProgressWithUnassignedWorkerBindsNull) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "dw"));
    TaskInfo t = c.value();
    t.worker_id = -1;  // unassigned → NULL
    t.current_phase = "";  // empty phase → BindNullableText null arm
    ASSERT_TRUE(mgr_.UpdateProgress(t).ok());
    auto got = mgr_.GetTask(t.task_id);
    EXPECT_EQ(got.value().worker_id, -1);
    EXPECT_TRUE(got.value().current_phase.empty());
}

// ---- Scheduler-support queries (Issue 2) ------------------------------------

TEST_F(TaskManagerTest, FindRecentTaskByDocIdWithinWindow) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "docX", "hashX"));
    ASSERT_TRUE(c.ok());
    auto recent = mgr_.FindRecentTaskByDocId("ns1", "docX", kTaskDocParse, 5);
    ASSERT_TRUE(recent.ok());
    ASSERT_TRUE(recent.value().has_value());
    EXPECT_EQ(recent.value()->task_id, c.value().task_id);
    EXPECT_EQ(recent.value()->content_hash, "hashX");
    // unknown doc → none
    auto none = mgr_.FindRecentTaskByDocId("ns1", "docY", kTaskDocParse, 5);
    ASSERT_TRUE(none.ok());
    EXPECT_FALSE(none.value().has_value());
    // empty doc_id → none (defensive)
    auto empty = mgr_.FindRecentTaskByDocId("ns1", "", kTaskDocParse, 5);
    ASSERT_TRUE(empty.ok());
    EXPECT_FALSE(empty.value().has_value());
}

TEST_F(TaskManagerTest, FindRecentTaskByDocIdScopesByTaskType) {
    // Same doc_id, two task kinds: the debounce lookup must NOT cross task_type
    // (doc-parse vs F41 doc-summary) so each finds only its own.
    auto parse = mgr_.CreateTask(MakeTask("ns1", "docZ", "hashZ"));  // default kTaskDocParse
    ASSERT_TRUE(parse.ok());
    TaskInfo summary_task = MakeTask("ns1", "docZ", "hashZ");
    summary_task.task_type = kTaskDocSummary;
    auto summary = mgr_.CreateTask(std::move(summary_task));
    ASSERT_TRUE(summary.ok());

    auto by_parse = mgr_.FindRecentTaskByDocId("ns1", "docZ", kTaskDocParse, 5);
    ASSERT_TRUE(by_parse.ok() && by_parse.value().has_value());
    EXPECT_EQ(by_parse.value()->task_id, parse.value().task_id);

    auto by_summary = mgr_.FindRecentTaskByDocId("ns1", "docZ", kTaskDocSummary, 5);
    ASSERT_TRUE(by_summary.ok() && by_summary.value().has_value());
    EXPECT_EQ(by_summary.value()->task_id, summary.value().task_id);

    // A task_type with no row → none, even though docZ has tasks of other kinds.
    auto by_other = mgr_.FindRecentTaskByDocId("ns1", "docZ", kTaskWatcherFanout, 5);
    ASSERT_TRUE(by_other.ok());
    EXPECT_FALSE(by_other.value().has_value());
}

TEST_F(TaskManagerTest, UpdateTaskForDebounceResetsProgress) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "docX", "oldhash"));
    // simulate it had progressed + failed
    TaskInfo p = c.value();
    p.processed_pages = 100;
    p.progress_pct = 20.0f;
    p.worker_id = 1;
    mgr_.UpdateProgress(p);

    SubmitRequest req;
    req.content_hash = "newhash";
    req.filepath = "/tmp/new.pdf";
    req.total_pages = 600;
    auto upd = mgr_.UpdateTaskForDebounce(c.value().task_id, req);
    ASSERT_TRUE(upd.ok());
    EXPECT_EQ(upd.value().content_hash, "newhash");
    EXPECT_EQ(upd.value().filepath, "/tmp/new.pdf");
    EXPECT_EQ(upd.value().status, std::string(task_status::kQueued));
    EXPECT_EQ(upd.value().processed_pages, 0);
    EXPECT_FLOAT_EQ(upd.value().progress_pct, 0.0f);
    EXPECT_EQ(upd.value().worker_id, -1);
    EXPECT_EQ(upd.value().total_pages, 600);
}

TEST_F(TaskManagerTest, SelectOldestQueuedRespectsActiveDocIdMutex) {
    auto t1 = mgr_.CreateTask(MakeTask("ns1", "docA"));
    auto t2 = mgr_.CreateTask(MakeTask("ns1", "docB"));
    ASSERT_TRUE(t1.ok());
    ASSERT_TRUE(t2.ok());

    // No active docs → oldest (docA) is picked.
    auto pick1 = mgr_.SelectOldestQueuedTaskExcluding({});
    ASSERT_TRUE(pick1.ok());
    ASSERT_TRUE(pick1.value().has_value());
    EXPECT_EQ(pick1.value()->doc_id, "docA");

    // docA active → docB is next (Issue 2.3 per-doc_id mutex).
    auto pick2 = mgr_.SelectOldestQueuedTaskExcluding({{"ns1", "docA"}});
    ASSERT_TRUE(pick2.ok());
    ASSERT_TRUE(pick2.value().has_value());
    EXPECT_EQ(pick2.value()->doc_id, "docB");

    // both active → nothing eligible.
    auto pick3 = mgr_.SelectOldestQueuedTaskExcluding({{"ns1", "docA"}, {"ns1", "docB"}});
    ASSERT_TRUE(pick3.ok());
    EXPECT_FALSE(pick3.value().has_value());
}

TEST_F(TaskManagerTest, SelectOldestQueuedSkipsNonQueued) {
    auto t1 = mgr_.CreateTask(MakeTask("ns1", "docA"));
    mgr_.MarkProcessing(t1.value().task_id, 1);  // no longer queued
    auto t2 = mgr_.CreateTask(MakeTask("ns1", "docB"));
    auto pick = mgr_.SelectOldestQueuedTaskExcluding({});
    ASSERT_TRUE(pick.ok());
    ASSERT_TRUE(pick.value().has_value());
    EXPECT_EQ(pick.value()->doc_id, "docB");
}

// A manager with a null db handle (never Init'd, or post-close) makes every
// sqlite3_prepare_v2 fail → each method returns a storage-failure Status rather
// than crashing. Covers the defensive prepare-failure branch of every op.
TEST(TaskManagerErrorTest, OperationsOnUninitializedManagerFailGracefully) {
    TaskManager m;  // no Init() → db_ == nullptr
    TaskInfo t;
    t.namespace_id = "ns";
    t.filename = "f";
    t.filepath = "/p";
    EXPECT_FALSE(m.CreateTask(t).ok());
    EXPECT_FALSE(m.GetTask("x").ok());
    EXPECT_FALSE(m.UpdateProgress(t).ok());
    EXPECT_FALSE(m.ListByNamespace("ns", 10, 0).ok());
    EXPECT_FALSE(m.MarkProcessing("x", 1).ok());
    EXPECT_FALSE(m.MarkCompleted("x", "d").ok());
    EXPECT_FALSE(m.MarkFailed("x", "c", "m").ok());
    EXPECT_FALSE(m.RequestCancel("x", nullptr).ok());
    EXPECT_FALSE(m.MarkCancelled("x").ok());
    EXPECT_FALSE(m.FindRecentTaskByDocId("ns1", "d", kTaskDocParse, 5).ok());
    SubmitRequest req;
    EXPECT_FALSE(m.UpdateTaskForDebounce("x", req).ok());
    EXPECT_FALSE(m.SelectOldestQueuedTaskExcluding({}).ok());
    EXPECT_FALSE(m.DeleteExpired(NowUnix(), 30).ok());
    EXPECT_FALSE(m.SweepZombies(NowUnix(), 24).ok());
    EXPECT_FALSE(m.RequeueStaleProcessing(NowUnix(), 24).ok());
    EXPECT_FALSE(m.CountAll().ok());
}

// ---- Cleanup (Issue 4) ------------------------------------------------------

TEST_F(TaskManagerTest, DeleteExpiredRemovesOldTerminalTasksOnly) {
    // Two completed tasks; backdate one's updated_at past the 30-day cutoff.
    auto keep = mgr_.CreateTask(MakeTask("ns1", "dkeep"));
    auto old = mgr_.CreateTask(MakeTask("ns1", "dold"));
    mgr_.MarkProcessing(keep.value().task_id, 1);
    mgr_.MarkProcessing(old.value().task_id, 2);
    mgr_.MarkCompleted(keep.value().task_id, "doc-keep");
    mgr_.MarkCompleted(old.value().task_id, "doc-old");

    // DeleteExpired uses updated_at < cutoff(now - 30d). Run with a future "now"
    // so the just-written rows are 'older than 30 days' relative to now+40d.
    int64_t now_plus_40d = NowUnix() + 40LL * 86400;
    auto del = mgr_.DeleteExpired(now_plus_40d, 30);
    ASSERT_TRUE(del.ok());
    EXPECT_EQ(del.value(), 2);  // both terminal + older than (now+40d - 30d)
    auto n = mgr_.CountAll();
    EXPECT_EQ(n.value(), 0);
}

TEST_F(TaskManagerTest, DeleteExpiredKeepsActiveTasks) {
    auto queued = mgr_.CreateTask(MakeTask("ns1", "dq"));
    auto proc = mgr_.CreateTask(MakeTask("ns1", "dp"));
    mgr_.MarkProcessing(proc.value().task_id, 1);
    // Even far in the future, queued/processing are never deleted by retention.
    int64_t now_plus_100d = NowUnix() + 100LL * 86400;
    auto del = mgr_.DeleteExpired(now_plus_100d, 30);
    ASSERT_TRUE(del.ok());
    EXPECT_EQ(del.value(), 0);
    EXPECT_EQ(mgr_.CountAll().value(), 2);
}

TEST_F(TaskManagerTest, SweepZombiesFlipsStaleProcessingToFailed) {
    auto z = mgr_.CreateTask(MakeTask("ns1", "dz"));
    mgr_.MarkProcessing(z.value().task_id, 1);
    // now+25h → the processing row (updated ~now) is older than 24h cutoff.
    int64_t now_plus_25h = NowUnix() + 25LL * 3600;
    auto swept = mgr_.SweepZombies(now_plus_25h, 24);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 1);
    auto got = mgr_.GetTask(z.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kFailed));
    EXPECT_EQ(got.value().error_code, "CX_ERR_ZOMBIE_TASK_CLEANUP");
}

TEST_F(TaskManagerTest, SweepZombiesLeavesFreshProcessingAlone) {
    auto p = mgr_.CreateTask(MakeTask("ns1", "dp"));
    mgr_.MarkProcessing(p.value().task_id, 1);
    auto swept = mgr_.SweepZombies(NowUnix(), 24);  // nothing older than 24h
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 0);
    EXPECT_EQ(mgr_.GetTask(p.value().task_id).value().status,
              std::string(task_status::kProcessing));
}

TEST_F(TaskManagerTest, RequeueStaleProcessingReturnsFreshToQueue) {
    auto p = mgr_.CreateTask(MakeTask("ns1", "dp"));
    mgr_.MarkProcessing(p.value().task_id, 3);
    // §6.1: a processing row younger than the 24h zombie threshold is re-queued
    // on restart (worker crashed but recent).
    auto requeued = mgr_.RequeueStaleProcessing(NowUnix(), 24);
    ASSERT_TRUE(requeued.ok());
    EXPECT_EQ(requeued.value(), 1);
    auto got = mgr_.GetTask(p.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kQueued));
    EXPECT_EQ(got.value().worker_id, -1);
}

TEST_F(TaskManagerTest, SweepTimedOutFlipsLongRunningToFailed) {
    auto p = mgr_.CreateTask(MakeTask("ns1", "dt"));
    mgr_.MarkProcessing(p.value().task_id, 1);  // started_at ~now
    // now+31min → started_at older than the 30-min (1800s) timeout cutoff (§7 v0).
    int64_t now_plus_31m = NowUnix() + 31LL * 60;
    auto swept = mgr_.SweepTimedOut(now_plus_31m, 1800);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 1);
    auto got = mgr_.GetTask(p.value().task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kFailed));
    EXPECT_EQ(got.value().error_code, "CX_ERR_TASK_TIMEOUT");
}

TEST_F(TaskManagerTest, SweepTimedOutLeavesFreshProcessingAlone) {
    auto p = mgr_.CreateTask(MakeTask("ns1", "dt2"));
    mgr_.MarkProcessing(p.value().task_id, 1);
    auto swept = mgr_.SweepTimedOut(NowUnix(), 1800);  // started ~now, within budget
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 0);
    EXPECT_EQ(mgr_.GetTask(p.value().task_id).value().status,
              std::string(task_status::kProcessing));
}

// A row whose created_at predates the debounce window is excluded (created_at >=
// cutoff is false): with a 0-second window, a just-created task is already stale.
TEST_F(TaskManagerTest, FindRecentTaskByDocIdOutsideWindowReturnsNone) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "docW", "hashW"));
    ASSERT_TRUE(c.ok());
    // window=-1s pushes the cutoff into the future so the row falls outside it.
    auto none = mgr_.FindRecentTaskByDocId("ns1", "docW", kTaskDocParse, -1);
    ASSERT_TRUE(none.ok());
    EXPECT_FALSE(none.value().has_value());
}

// SweepTimedOut ignores a processing row with NULL started_at (started_at IS NOT
// NULL predicate is false). UpdateProgress alone never sets started_at.
TEST_F(TaskManagerTest, SweepTimedOutIgnoresRowsWithNullStartedAt) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "dns"));
    // Move to processing-ish state via UpdateProgress without MarkProcessing, so
    // status stays 'queued' and started_at stays NULL — no timeout candidate.
    int64_t now_plus_1d = NowUnix() + 86400;
    auto swept = mgr_.SweepTimedOut(now_plus_1d, 1800);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 0);  // nothing in 'processing' with a started_at
    EXPECT_EQ(mgr_.GetTask(c.value().task_id).value().status,
              std::string(task_status::kQueued));
}

// Cleanup sweeps over an empty table return 0 changes (the changes==0 / no-match
// path of each DELETE/UPDATE) without error.
TEST_F(TaskManagerTest, CleanupSweepsOnEmptyTableReturnZero) {
    int64_t far = NowUnix() + 100LL * 86400;
    EXPECT_EQ(mgr_.DeleteExpired(far, 30).value(), 0);
    EXPECT_EQ(mgr_.SweepZombies(far, 24).value(), 0);
    EXPECT_EQ(mgr_.SweepTimedOut(far, 1800).value(), 0);
    EXPECT_EQ(mgr_.RequeueStaleProcessing(NowUnix(), 24).value(), 0);
}

// RequeueStaleProcessing leaves a row OLDER than the zombie threshold alone (the
// updated_at >= cutoff predicate is false) — those are SweepZombies' job.
TEST_F(TaskManagerTest, RequeueStaleProcessingLeavesOldRowsForZombieSweep) {
    auto p = mgr_.CreateTask(MakeTask("ns1", "drq"));
    mgr_.MarkProcessing(p.value().task_id, 1);
    // now+25h: the row (updated ~now) is older than the 24h cutoff → NOT requeued.
    int64_t now_plus_25h = NowUnix() + 25LL * 3600;
    auto requeued = mgr_.RequeueStaleProcessing(now_plus_25h, 24);
    ASSERT_TRUE(requeued.ok());
    EXPECT_EQ(requeued.value(), 0);
    EXPECT_EQ(mgr_.GetTask(p.value().task_id).value().status,
              std::string(task_status::kProcessing));
}

// ---- step-failure arms (rc != SQLITE_DONE) via a RAISE(ABORT) trigger ---------
//
// The null-db tests cover every prepare-failure arm, but the `rc != SQLITE_DONE`
// step-failure arm of each write needs a statement that prepares cleanly yet fails
// at step time. A temp-file db lets a second connection install BEFORE-write
// triggers that RAISE(ABORT); the manager's own connection then fails at step.
class TaskManagerFaultTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Unique temp path so the manager + the probe connection share real tables
        // (":memory:" is per-connection and could not be poked from outside).
        path_ = std::string(::testing::TempDir()) + "tm_fault_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::remove(path_.c_str());
        ASSERT_TRUE(mgr_.Init(path_).ok());
        ASSERT_EQ(sqlite3_open(path_.c_str(), &probe_), SQLITE_OK);
    }
    void TearDown() override {
        if (probe_) sqlite3_close(probe_);
        std::remove(path_.c_str());
        std::remove((path_ + "-wal").c_str());
        std::remove((path_ + "-shm").c_str());
    }
    void Exec(const char* sql) {
        ASSERT_EQ(sqlite3_exec(probe_, sql, nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(probe_);
    }
    std::string path_;
    TaskManager mgr_;
    sqlite3* probe_ = nullptr;
};

// A BEFORE INSERT trigger that aborts makes CreateTask's sqlite3_step fail
// (rc != SQLITE_DONE → storage-failure Status), exercised separately from the
// prepare-failure arm.
TEST_F(TaskManagerFaultTest, CreateTaskStepFailureIsStorageError) {
    Exec("CREATE TRIGGER t_block_ins BEFORE INSERT ON tasks "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END;");
    TaskInfo t;
    t.namespace_id = "ns1";
    t.filename = "f.pdf";
    t.filepath = "/tmp/f.pdf";
    auto r = mgr_.CreateTask(t);
    EXPECT_FALSE(r.ok());
}

// A BEFORE UPDATE trigger aborts → the step-failure arm of every UPDATE-based
// state transition (MarkProcessing / MarkCompleted / MarkFailed / MarkCancelled /
// UpdateProgress / UpdateTaskForDebounce). With the guarded Marks, each UPDATE
// must target a row in a LEGAL source state so its predicate matches and the
// trigger fires at step time — otherwise the call fails via the changes==0
// conflict arm instead (the wrong arm for this test). Two rows: one stays
// queued (MarkProcessing / MarkCancelled / UpdateProgress / debounce), one is
// moved to processing BEFORE the trigger is armed (MarkCompleted / MarkFailed).
TEST_F(TaskManagerFaultTest, UpdateStepFailuresAreStorageErrors) {
    auto a = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(a.ok());
    const std::string qid = a.value().task_id;  // stays queued
    auto b = mgr_.CreateTask(MakeTask("ns1", "doc2", "h2"));
    ASSERT_TRUE(b.ok());
    const std::string pid = b.value().task_id;
    ASSERT_TRUE(mgr_.MarkProcessing(pid, 1).ok());  // legal source for completed/failed
    Exec("CREATE TRIGGER t_block_upd BEFORE UPDATE ON tasks "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END;");

    auto expect_storage_error = [](const Status& s) {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_STORAGE_FAILED"), std::string::npos)
            << "expected the step-failure arm, got: " << s.message();
    };
    expect_storage_error(mgr_.MarkProcessing(qid, 1));
    expect_storage_error(mgr_.MarkCompleted(pid, "d"));
    expect_storage_error(mgr_.MarkFailed(pid, "CX_ERR_PARSE_FAILED", "x"));
    expect_storage_error(mgr_.MarkCancelled(qid));
    TaskInfo upd = a.value();
    upd.processed_pages = 1;
    EXPECT_FALSE(mgr_.UpdateProgress(upd).ok());
    SubmitRequest req;
    req.content_hash = "h";
    req.filepath = "/tmp/x.pdf";
    EXPECT_FALSE(mgr_.UpdateTaskForDebounce(qid, req).ok());
}

// The cleanup UPDATE sweeps (SweepZombies / SweepTimedOut / RequeueStaleProcessing)
// only touch status='processing' rows, so the probe first flips the seeded row to
// processing (with a started_at) before the abort trigger is armed — then each
// sweep matches a row and fails at step.
TEST_F(TaskManagerFaultTest, CleanupSweepStepFailuresAreStorageErrors) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(c.ok());
    // Force the row into processing with an old started_at/updated_at directly, so
    // all three sweeps consider it a candidate.
    Exec("UPDATE tasks SET status='processing', "
         "started_at='2020-01-01T00:00:00.000Z', "
         "updated_at='2020-01-01T00:00:00.000Z';");
    Exec("CREATE TRIGGER t_block_sweep BEFORE UPDATE ON tasks "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END;");

    EXPECT_FALSE(mgr_.SweepZombies(NowUnix(), 24).ok());
    EXPECT_FALSE(mgr_.SweepTimedOut(NowUnix(), 1800).ok());
    // RequeueStaleProcessing targets rows updated_at >= cutoff (fresh-ish); the
    // 2020 row is older than any realistic cutoff, so it would NOT match. Use a
    // far-past zombie window so cutoff predates 2020 and the row qualifies.
    EXPECT_FALSE(mgr_.RequeueStaleProcessing(NowUnix(), 24LL * 365 * 100).ok());
}

// RequestCancel reads the row, then runs an UPDATE: a BEFORE UPDATE abort drives
// the update-step-failure arm (distinct from the not-found / 423 arms).
TEST_F(TaskManagerFaultTest, RequestCancelUpdateStepFailureIsStorageError) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(c.ok());
    Exec("CREATE TRIGGER t_block_upd2 BEFORE UPDATE ON tasks "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END;");
    auto s = mgr_.RequestCancel(c.value().task_id, nullptr);  // queued → would cancel
    EXPECT_FALSE(s.ok());
}

// A BEFORE DELETE trigger aborts → DeleteExpired's step-failure arm. The row
// must actually be terminal (DeleteExpired only matches terminal statuses), so
// go through the legal processing -> completed path and assert both writes.
TEST_F(TaskManagerFaultTest, DeleteExpiredStepFailureIsStorageError) {
    auto c = mgr_.CreateTask(MakeTask("ns1", "doc1"));
    ASSERT_TRUE(c.ok());
    ASSERT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
    ASSERT_TRUE(mgr_.MarkCompleted(c.value().task_id, "d").ok());
    Exec("CREATE TRIGGER t_block_del BEFORE DELETE ON tasks "
         "BEGIN SELECT RAISE(ABORT, 'blocked'); END;");
    auto del = mgr_.DeleteExpired(NowUnix() + 100LL * 86400, 30);
    EXPECT_FALSE(del.ok());
}

// ---- FailedPagesFromJson tolerance branches (via stored malformed JSON) --------
//
// ReadRow parses failed_pages through FailedPagesFromJson, whose non-array and
// non-integer-element arms are unreachable from the normal write path (which only
// stores a clean int array). Inject malformed values directly, then read back.
TEST_F(TaskManagerFaultTest, FailedPagesFromJsonToleratesMalformedStored) {
    // A row whose failed_pages is a JSON object (not an array) → parsed.is_array()
    // is false → FromJson returns empty.
    Exec("INSERT INTO tasks(task_id, namespace_id, filename, filepath, status, "
         "failed_pages, created_at, updated_at) "
         "VALUES('obj','ns','f','/p','queued','{\"a\":1}','t','t');");
    // A row whose failed_pages array mixes a non-integer element → that element is
    // skipped (the is_number_integer() false arm), the integer is kept.
    Exec("INSERT INTO tasks(task_id, namespace_id, filename, filepath, status, "
         "failed_pages, created_at, updated_at) "
         "VALUES('mix','ns','f','/p','queued','[1,\"x\",2]','t','t');");

    // An empty failed_pages string → the `s.empty()` early-return arm of
    // FailedPagesFromJson (CreateTask always stores "[]", never ""), so this arm is
    // only reachable from a directly-written empty value.
    Exec("INSERT INTO tasks(task_id, namespace_id, filename, filepath, status, "
         "failed_pages, created_at, updated_at) "
         "VALUES('empty','ns','f','/p','queued','','t','t');");

    auto obj = mgr_.GetTask("obj");
    ASSERT_TRUE(obj.ok());
    EXPECT_TRUE(obj.value().failed_pages.empty());  // object → no pages

    auto mix = mgr_.GetTask("mix");
    ASSERT_TRUE(mix.ok());
    EXPECT_EQ(mix.value().failed_pages, (std::vector<int>{1, 2}));  // "x" dropped

    auto empty = mgr_.GetTask("empty");
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty.value().failed_pages.empty());  // "" → empty early-return
}

// Init on a path that cannot be opened drives the sqlite3_open failure arm
// (rc != SQLITE_OK → db_ reset to null, storage-failure Status). A path under a
// non-existent directory is unopenable.
TEST(TaskManagerInitFault, OpenFailureIsStorageError) {
    TaskManager mgr;
    auto st = mgr.Init("/nonexistent_dir_xyz/sub/tasks.db");
    EXPECT_FALSE(st.ok());
}

// ---- CreateTasksTable ExecSQL failure arm (Init on a broken handle) -----------
//
// ExecSQL's `rc != SQLITE_OK` arm fires when the CREATE-TABLE batch fails. A
// pre-existing index named `tasks` defeats `CREATE TABLE IF NOT EXISTS tasks`, so
// CreateTasksTable returns a storage-failure Status.
TEST(TaskManagerCreateTableFault, CreateTasksTableExecFailureSurfaces) {
    const std::string path = std::string(::testing::TempDir()) + "tm_createtable_fault.db";
    std::remove(path.c_str());
    sqlite3* seed = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &seed), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(seed, "CREATE TABLE helper(x INTEGER); "
                                 "CREATE INDEX tasks ON helper(x);",
                           nullptr, nullptr, nullptr), SQLITE_OK)
        << sqlite3_errmsg(seed);
    sqlite3_close(seed);

    TaskManager mgr;
    auto st = mgr.Init(path);  // CreateTasksTable's CREATE TABLE tasks collides
    EXPECT_FALSE(st.ok());

    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

}  // namespace
}  // namespace cortrix::async
