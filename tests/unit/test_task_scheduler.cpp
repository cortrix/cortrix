#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/common/in_memory_global_config.h"

// S2 coverage: TaskScheduler — Issue 2.2 Watcher debounce (merge / reset), Issue 2.1
// FIFO queueing, Issue 2.3 per-doc_id mutex via active_doc_ids_.
namespace cortrix::async {
namespace {

SubmitRequest MakeReq(const std::string& doc, const std::string& hash,
                      const std::string& path = "/tmp/f.pdf") {
    SubmitRequest r;
    r.namespace_id = "ns1";
    r.filename = "f.pdf";
    r.filepath = path;
    r.doc_id = doc;
    r.content_hash = hash;
    r.total_pages = 300;
    return r;
}

class TaskSchedulerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        sched_ = std::make_unique<TaskScheduler>(&mgr_, &cfg_);
    }
    TaskManager mgr_;
    InMemoryGlobalConfig cfg_;
    std::unique_ptr<TaskScheduler> sched_;
};

TEST_F(TaskSchedulerTest, EnqueueCreatesQueuedTask) {
    auto r = sched_->Enqueue(MakeReq("docA", "h1"));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().status, std::string(task_status::kQueued));
    EXPECT_EQ(r.value().doc_id, "docA");
    EXPECT_EQ(mgr_.CountAll().value(), 1);
}

TEST_F(TaskSchedulerTest, DebounceMergesSameDocSameHash) {
    auto first = sched_->Enqueue(MakeReq("docA", "h1"));
    ASSERT_TRUE(first.ok());
    // same doc_id + same content_hash within 5s → merged (returns existing, no new row).
    auto dup = sched_->Enqueue(MakeReq("docA", "h1"));
    ASSERT_TRUE(dup.ok());
    EXPECT_EQ(dup.value().task_id, first.value().task_id);
    EXPECT_EQ(mgr_.CountAll().value(), 1);  // no second row
}

TEST_F(TaskSchedulerTest, DebounceResetsSameDocDifferentHash) {
    auto first = sched_->Enqueue(MakeReq("docA", "h1", "/tmp/v1.pdf"));
    ASSERT_TRUE(first.ok());
    // simulate progress so we can see the reset
    TaskInfo p = first.value();
    p.processed_pages = 50;
    p.progress_pct = 16.6f;
    mgr_.UpdateProgress(p);

    // same doc_id + DIFFERENT content_hash within 5s → reuse row, refresh + reset.
    auto upd = sched_->Enqueue(MakeReq("docA", "h2", "/tmp/v2.pdf"));
    ASSERT_TRUE(upd.ok());
    EXPECT_EQ(upd.value().task_id, first.value().task_id);  // same row reused
    EXPECT_EQ(upd.value().content_hash, "h2");
    EXPECT_EQ(upd.value().filepath, "/tmp/v2.pdf");
    EXPECT_EQ(upd.value().processed_pages, 0);  // progress reset
    EXPECT_EQ(upd.value().status, std::string(task_status::kQueued));
    EXPECT_EQ(mgr_.CountAll().value(), 1);
}

TEST_F(TaskSchedulerTest, NoDebounceWhenWindowDisabled) {
    cfg_.Set("async.watcher_debounce_seconds", "0");  // window 0 → never merge
    auto a = sched_->Enqueue(MakeReq("docA", "h1"));
    auto b = sched_->Enqueue(MakeReq("docA", "h1"));
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_NE(a.value().task_id, b.value().task_id);  // two distinct rows
    EXPECT_EQ(mgr_.CountAll().value(), 2);
}

TEST_F(TaskSchedulerTest, DequeueMarksProcessingAndReservesDoc) {
    auto e = sched_->Enqueue(MakeReq("docA", "h1"));
    ASSERT_TRUE(e.ok());
    auto dq = sched_->Dequeue(1);
    ASSERT_TRUE(dq.ok());
    ASSERT_TRUE(dq.value().has_value());
    EXPECT_EQ(dq.value()->status, std::string(task_status::kProcessing));
    EXPECT_EQ(dq.value()->worker_id, 1);
    EXPECT_TRUE(sched_->IsDocActive("ns1", "docA"));
    EXPECT_EQ(sched_->ActiveDocCount(), 1u);
}

TEST_F(TaskSchedulerTest, PerDocIdMutexDefersSameDoc) {
    // Issue 2.1/2.3: two tasks for the SAME doc_id; only one can be active at once.
    sched_->Enqueue(MakeReq("docA", "h1"));
    cfg_.Set("async.watcher_debounce_seconds", "0");  // avoid merge so we get 2 rows
    sched_->Enqueue(MakeReq("docA", "h2"));

    auto first = sched_->Dequeue(1);
    ASSERT_TRUE(first.value().has_value());
    EXPECT_EQ(first.value()->doc_id, "docA");

    // docA now active → second Dequeue finds nothing (the other docA task is deferred).
    auto second = sched_->Dequeue(2);
    ASSERT_TRUE(second.ok());
    EXPECT_FALSE(second.value().has_value());

    // Release docA → the deferred task becomes dequeuable.
    sched_->OnTaskCompleted("ns1", "docA");
    EXPECT_FALSE(sched_->IsDocActive("ns1", "docA"));
    auto third = sched_->Dequeue(2);
    ASSERT_TRUE(third.value().has_value());
    EXPECT_EQ(third.value()->doc_id, "docA");
}

TEST_F(TaskSchedulerTest, DequeueDifferentDocsConcurrently) {
    sched_->Enqueue(MakeReq("docA", "h1"));
    sched_->Enqueue(MakeReq("docB", "h2"));
    auto a = sched_->Dequeue(1);
    auto b = sched_->Dequeue(2);
    ASSERT_TRUE(a.value().has_value());
    ASSERT_TRUE(b.value().has_value());
    EXPECT_NE(a.value()->doc_id, b.value()->doc_id);  // different docs both run
    EXPECT_EQ(sched_->ActiveDocCount(), 2u);
}

TEST_F(TaskSchedulerTest, DequeueEmptyQueueReturnsNullopt) {
    auto dq = sched_->Dequeue(1);
    ASSERT_TRUE(dq.ok());
    EXPECT_FALSE(dq.value().has_value());
}

TEST_F(TaskSchedulerTest, OnTaskCompletedIsIdempotent) {
    sched_->Enqueue(MakeReq("docA", "h1"));
    sched_->Dequeue(1);
    sched_->OnTaskCompleted("ns1", "docA");
    sched_->OnTaskCompleted("ns1", "docA");  // no-op second release
    EXPECT_EQ(sched_->ActiveDocCount(), 0u);
}

// ---- additional branch coverage --------------------------------------------

// A null IGlobalConfig makes DebounceSeconds() fall back to the default window
// (config_ == nullptr branch), and debounce still merges identical resubmissions.
TEST_F(TaskSchedulerTest, NullConfigUsesDefaultDebounce) {
    TaskScheduler sched(&mgr_, /*config=*/nullptr);
    auto a = sched.Enqueue(MakeReq("docN", "h1"));
    auto b = sched.Enqueue(MakeReq("docN", "h1"));  // default 5s window → merged
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().task_id, b.value().task_id);
    EXPECT_EQ(mgr_.CountAll().value(), 1);
}

// A config present but missing the key leaves DebounceSeconds() on the default
// (GetInt not ok branch): debounce remains active.
TEST_F(TaskSchedulerTest, MissingConfigKeyUsesDefaultDebounce) {
    // cfg_ has no "async.watcher_debounce_seconds" set -> GetInt fails -> default.
    auto a = sched_->Enqueue(MakeReq("docK", "h1"));
    auto b = sched_->Enqueue(MakeReq("docK", "h1"));
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().task_id, b.value().task_id);  // merged under default window
}

// Enqueue with an empty doc_id skips the debounce lookup entirely (the
// !req.doc_id.empty() guard is false) and always creates a fresh row.
TEST_F(TaskSchedulerTest, EnqueueEmptyDocIdAlwaysCreatesNewRow) {
    auto a = sched_->Enqueue(MakeReq("", "h1"));
    auto b = sched_->Enqueue(MakeReq("", "h1"));
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_NE(a.value().task_id, b.value().task_id);
    EXPECT_EQ(mgr_.CountAll().value(), 2);
}

// OnTaskCompleted("ns1", "") with an empty doc_id is a no-op on the active set (the
// !doc_id.empty() guard is false) but still refreshes the queue-depth gauge.
TEST_F(TaskSchedulerTest, OnTaskCompletedEmptyDocIdIsNoOp) {
    sched_->Enqueue(MakeReq("docA", "h1"));
    sched_->Dequeue(1);
    ASSERT_EQ(sched_->ActiveDocCount(), 1u);
    sched_->OnTaskCompleted("ns1", "");  // empty → does not erase docA
    EXPECT_EQ(sched_->ActiveDocCount(), 1u);
}

// IsDocActive returns false for a doc that was never dequeued (the count()==0
// branch), complementing the active-doc assertions above.
TEST_F(TaskSchedulerTest, IsDocActiveFalseForUnknownDoc) {
    EXPECT_FALSE(sched_->IsDocActive("ns1", "never-seen"));
    sched_->Enqueue(MakeReq("docA", "h1"));
    EXPECT_FALSE(sched_->IsDocActive("ns1", "docA"));  // enqueued but not yet dequeued
}

// Dequeue reserves the doc_id, then MarkProcessing fails → the rollback arm erases
// the reservation and returns the error (lines: active_doc_ids_.erase + return s).
// A temp-file db + a probe BEFORE UPDATE abort trigger forces MarkProcessing to
// fail at step after SelectOldestQueuedTaskExcluding has already picked the row.
TEST(TaskSchedulerDequeueFaultTest, MarkProcessingFailureRollsBackReservation) {
    const std::string path = std::string(::testing::TempDir()) + "sched_dq_fault.db";
    std::remove(path.c_str());
    TaskManager mgr;
    ASSERT_TRUE(mgr.Init(path).ok());
    InMemoryGlobalConfig cfg;
    TaskScheduler sched(&mgr, &cfg);

    SubmitRequest req;
    req.namespace_id = "ns1";
    req.filename = "f.pdf";
    req.filepath = "/tmp/f.pdf";
    req.doc_id = "docA";
    req.content_hash = "h1";
    req.total_pages = 300;
    ASSERT_TRUE(sched.Enqueue(req).ok());

    sqlite3* probe = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &probe), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(probe,
        "CREATE TRIGGER t_block_mp BEFORE UPDATE ON tasks "
        "BEGIN SELECT RAISE(ABORT,'blocked'); END;", nullptr, nullptr, nullptr),
        SQLITE_OK) << sqlite3_errmsg(probe);

    auto dq = sched.Dequeue(1);  // picks docA, reserves it, MarkProcessing aborts
    EXPECT_FALSE(dq.ok());
    EXPECT_FALSE(sched.IsDocActive("ns1", "docA"));  // reservation rolled back
    EXPECT_EQ(sched.ActiveDocCount(), 0u);

    sqlite3_close(probe);
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

}  // namespace
}  // namespace cortrix::async
