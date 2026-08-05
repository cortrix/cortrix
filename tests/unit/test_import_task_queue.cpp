#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "cortrix/import/import_error.h"
#include "cortrix/import/import_task_queue.h"

// S4 coverage: the self-built D6 ImportTaskQueue — submit / progress /
// cancel state machine + import_tasks persistence + worker pool. Built ON the shared
// cortrix::ExecutorEngine, with ZERO async task dependency (§0 red line). Work is a per-task
// closure (the ImportManager captures its auth/trace ctx). Runs against the in-memory
// task store (standalone; SQLite-backed store → D3.5).
namespace cortrix::import {
namespace {

// Spin until `pred()` or a generous deadline (worker threads are async).
template <typename Pred>
bool WaitFor(Pred pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

TEST(ImportTaskQueueTest, SubmitRunsToCompleted) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    std::atomic<int> ran{0};
    ImportTaskQueue q(store);

    auto r = q.Submit("import_1", "customer_kb",
                      [&](ImportTaskHandle& h) {
                          h.ReportProgress(10, 10);
                          ran.fetch_add(1);
                          return Status::Ok();
                      },
                      /*rows_total=*/10);
    ASSERT_TRUE(r.ok());
    q.DrainForTest();

    auto p = q.GetProgress("import_1");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, ImportTaskStatus::kCompleted);
    EXPECT_DOUBLE_EQ(p->progress, 1.0);
    EXPECT_EQ(p->rows_imported, 10);
    EXPECT_EQ(ran.load(), 1);
    EXPECT_FALSE(p->error.has_value());
    EXPECT_TRUE(p->started_at.has_value());
    EXPECT_TRUE(p->completed_at.has_value());
}

TEST(ImportTaskQueueTest, FailedWorkPropagatesCxErrCode) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueue q(store);
    ASSERT_TRUE(q.Submit("import_fail", "ns", [&](ImportTaskHandle&) {
                     return ImportStatus(ImportErrorCode::kTimeout,
                                       "Query exceeded 300s timeout on table users");
                 }).ok());
    q.DrainForTest();

    auto p = q.GetProgress("import_fail");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, ImportTaskStatus::kFailed);
    ASSERT_TRUE(p->error.has_value());
    // the CX_ERR_IMPORT_* identity is recovered from the Status message prefix.
    EXPECT_EQ(p->error->code, "CX_ERR_IMPORT_TIMEOUT");
}

TEST(ImportTaskQueueTest, CancelWhileQueuedNeverRunsWork) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    std::atomic<int> ran{0};
    std::atomic<bool> gate{false};
    ImportTaskQueueConfig cfg;
    cfg.worker_count = 1;  // a single worker, so the 2nd task stays QUEUED
    ImportTaskQueue q(store, cfg);

    auto busy_work = [&](ImportTaskHandle&) {
        ran.fetch_add(1);
        WaitFor([&] { return gate.load(); }, 2000);
        return Status::Ok();
    };
    ASSERT_TRUE(q.Submit("import_busy", "ns", busy_work).ok());  // occupies the worker
    ASSERT_TRUE(WaitFor([&] { return ran.load() == 1; }));

    auto queued_work = [&](ImportTaskHandle&) {
        ran.fetch_add(1);
        return Status::Ok();
    };
    ASSERT_TRUE(q.Submit("import_queued", "ns", queued_work).ok());  // waits behind it

    EXPECT_TRUE(q.Cancel("import_queued"));
    auto pq = q.GetProgress("import_queued");
    ASSERT_TRUE(pq.has_value());
    EXPECT_EQ(pq->status, ImportTaskStatus::kCancelled);  // QUEUED → CANCELLED directly

    gate.store(true);  // release the worker
    q.DrainForTest();
    EXPECT_EQ(ran.load(), 1);  // the cancelled task's work never ran
}

TEST(ImportTaskQueueTest, CancelWhileRunningStopsCooperatively) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    std::atomic<bool> entered{false};
    ImportTaskQueue q(store);
    ASSERT_TRUE(q.Submit("import_run", "ns", [&](ImportTaskHandle& h) {
                     entered.store(true);
                     for (int i = 0; i < 100000; ++i) {
                         if (h.CancelRequested()) return Status::Ok();  // stop, leave partial
                         std::this_thread::sleep_for(std::chrono::milliseconds(1));
                     }
                     return Status::Ok();
                 }).ok());
    ASSERT_TRUE(WaitFor([&] { return entered.load(); }));

    EXPECT_TRUE(q.Cancel("import_run"));
    // status goes RUNNING → CANCELLING, then the worker stops and finalizes CANCELLED.
    EXPECT_TRUE(WaitFor([&] {
        auto p = q.GetProgress("import_run");
        return p && p->status == ImportTaskStatus::kCancelled;
    }));
    q.DrainForTest();
    auto p = q.GetProgress("import_run");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, ImportTaskStatus::kCancelled);
    EXPECT_FALSE(p->error.has_value());  // cancel is not an error (§5.4 note)
}

TEST(ImportTaskQueueTest, ProgressRatioTracksRows) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    std::atomic<bool> hold{true};
    ImportTaskQueue q(store);
    ASSERT_TRUE(q.Submit("import_prog", "ns", [&](ImportTaskHandle& h) {
                     h.ReportProgress(45, 100);
                     WaitFor([&] { return !hold.load(); }, 2000);
                     h.ReportProgress(100, 100);
                     return Status::Ok();
                 }, 100).ok());

    EXPECT_TRUE(WaitFor([&] {
        auto p = q.GetProgress("import_prog");
        return p && p->rows_imported == 45;
    }));
    auto mid = q.GetProgress("import_prog");
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(mid->progress, 0.45, 1e-9);
    EXPECT_EQ(mid->status, ImportTaskStatus::kRunning);

    hold.store(false);
    q.DrainForTest();
    auto fin = q.GetProgress("import_prog");
    EXPECT_DOUBLE_EQ(fin->progress, 1.0);
}

TEST(ImportTaskQueueTest, CancelUnknownTaskReturnsFalse) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueue q(store);
    EXPECT_FALSE(q.Cancel("does_not_exist"));
    EXPECT_FALSE(q.GetProgress("does_not_exist").has_value());
}

// --- store-level CAS (the basis of the cancel/worker race) ---

TEST(ImportTaskStoreTest, CompareAndSetStatusIsConditional) {
    InMemoryImportTaskStore store;
    ImportTaskProgress t;
    t.task_id = "t1";
    t.status = ImportTaskStatus::kQueued;
    ASSERT_TRUE(store.Create(t).ok());

    EXPECT_FALSE(store.CompareAndSetStatus("t1", ImportTaskStatus::kRunning,
                                           ImportTaskStatus::kCompleted));
    EXPECT_EQ(store.Get("t1")->status, ImportTaskStatus::kQueued);
    EXPECT_TRUE(store.CompareAndSetStatus("t1", ImportTaskStatus::kQueued,
                                          ImportTaskStatus::kRunning));
    EXPECT_EQ(store.Get("t1")->status, ImportTaskStatus::kRunning);
    EXPECT_FALSE(store.CompareAndSetStatus("ghost", ImportTaskStatus::kQueued,
                                           ImportTaskStatus::kRunning));
}

TEST(ImportTaskStoreTest, DuplicateCreateRejected) {
    InMemoryImportTaskStore store;
    ImportTaskProgress t;
    t.task_id = "t1";
    EXPECT_TRUE(store.Create(t).ok());
    EXPECT_FALSE(store.Create(t).ok());
}

TEST(ImportTaskStoreTest, ListByNamespaceFilters) {
    InMemoryImportTaskStore store;
    ImportTaskProgress a;
    a.task_id = "a";
    a.namespace_id = "ns1";
    ImportTaskProgress b;
    b.task_id = "b";
    b.namespace_id = "ns2";
    ASSERT_TRUE(store.Create(a).ok());
    ASSERT_TRUE(store.Create(b).ok());
    EXPECT_EQ(store.ListByNamespace("ns1").size(), 1u);
    EXPECT_EQ(store.ListByNamespace("ns2").size(), 1u);
    EXPECT_EQ(store.ListByNamespace("ns3").size(), 0u);
}

}  // namespace
}  // namespace cortrix::import
