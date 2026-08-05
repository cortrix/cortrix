#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>

#include "cortrix/async/document_task_handler.h"
#include "cortrix/async/task_error.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/async/worker_pool.h"
#include "cortrix/common/in_memory_global_config.h"

// S3 coverage: DocumentTaskHandler — POST async-decision/enqueue, GET progress
// (§6.3 body + meta), DELETE cancel (§4.3), and the Issue 5 GEN-Agent 4-field error
// envelope across the 11 codes. No WorkerPool runs here (pool=nullptr), so tasks
// stay queued and progress/cancel are observed deterministically.
namespace cortrix::async {
namespace {

SubmitParams MakeParams(const std::string& doc, int pages, bool ent) {
    SubmitParams p;
    p.namespace_id = "ns1";
    p.filename = "big.pdf";
    p.filepath = "/srv/tmp/big.pdf";  // not stat()ed here (no parse runs in S3)
    p.doc_id = doc;
    p.content_hash = doc + "-h";
    p.page_count = pages;
    p.async_overflow = ent;
    return p;
}

class DocumentTaskHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        sched_ = std::make_unique<TaskScheduler>(&mgr_, &cfg_);
        handler_ = std::make_unique<DocumentTaskHandler>(sched_.get(), &mgr_,
                                                         /*pool=*/nullptr, &cfg_);
    }
    TaskManager mgr_;
    InMemoryGlobalConfig cfg_;
    std::unique_ptr<TaskScheduler> sched_;
    std::unique_ptr<DocumentTaskHandler> handler_;
};

// ---- POST async decision (§2.2) -------------------------------------------

TEST_F(DocumentTaskHandlerTest, SmallDocRoutesToSyncPath) {
    auto r = handler_->SubmitAsync(MakeParams("docS", /*pages=*/50, /*ent=*/true));
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["status"], "sync_required");
    EXPECT_EQ(mgr_.CountAll().value(), 0);  // no async task minted
}

TEST_F(DocumentTaskHandlerTest, AsyncOverflowLargeDocMintsAsyncTask) {
    auto r = handler_->SubmitAsync(MakeParams("docL", /*pages=*/800, /*ent=*/true));
    EXPECT_EQ(r.status, 202);
    EXPECT_EQ(r.body["status"], "processing");
    ASSERT_TRUE(r.body.contains("task_id"));
    EXPECT_EQ(r.body["task_id"].get<std::string>().size(), 26u);  // ULID
    EXPECT_TRUE(r.body.contains("accepted_at"));
    EXPECT_EQ(mgr_.CountAll().value(), 1);
}

TEST_F(DocumentTaskHandlerTest, CeLargeDocRejected403MaxPages) {
    auto r = handler_->SubmitAsync(MakeParams("docC", /*pages=*/800, /*ent=*/false));
    EXPECT_EQ(r.status, 403);  // Issue 1.3 A — CE sync-only
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_MAX_PAGES_EXCEEDED");
    EXPECT_EQ(r.body["error"]["category"], "permanent");
    EXPECT_EQ(r.body["error"]["retryable"], false);
    EXPECT_EQ(r.body["error"]["structured_data"]["async_overflow"], false);
    EXPECT_EQ(mgr_.CountAll().value(), 0);
}

TEST_F(DocumentTaskHandlerTest, OverEntCapRejectedForEveryone) {
    cfg_.Set("f42.async_max_pages", "2000");
    auto r = handler_->SubmitAsync(MakeParams("docHuge", /*pages=*/3000, /*ent=*/true));
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_MAX_PAGES_EXCEEDED");
    EXPECT_EQ(r.body["error"]["structured_data"]["max_pages"], 2000);
    EXPECT_EQ(r.body["error"]["structured_data"]["actual_pages"], 3000);
}

TEST_F(DocumentTaskHandlerTest, MissingFieldsRejected400) {
    SubmitParams p = MakeParams("docX", 800, true);
    p.filepath = "";  // missing
    auto r = handler_->SubmitAsync(p);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_INVALID_REQUEST");
    ASSERT_TRUE(r.body["error"]["structured_data"].contains("rules_violated"));
}

// The 400 guard is a 3-way OR; MissingFieldsRejected400 only trips the filepath
// arm. Exercise the namespace_id-empty and filename-empty arms independently.
TEST_F(DocumentTaskHandlerTest, MissingNamespaceRejected400) {
    SubmitParams p = MakeParams("docX", 800, true);
    p.namespace_id = "";
    auto r = handler_->SubmitAsync(p);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_INVALID_REQUEST");
}

TEST_F(DocumentTaskHandlerTest, MissingFilenameRejected400) {
    SubmitParams p = MakeParams("docX", 800, true);
    p.filename = "";
    auto r = handler_->SubmitAsync(p);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_INVALID_REQUEST");
}

// Over the hard cap with async overflow enabled: the policy ternary's
// async-overflow arm (round-1 used the sync-only path; this pins the
// async_overflow==true label explicitly).
TEST_F(DocumentTaskHandlerTest, OverHardCapLabelsAsyncOverflow) {
    cfg_.Set("f42.async_max_pages", "1000");
    auto r = handler_->SubmitAsync(MakeParams("docHugeEnt", 5000, /*ent=*/true));
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(r.body["error"]["structured_data"]["async_overflow"], true);
}

// Over the Ent hard cap as a CE caller: the edition ternary's "CE" arm.
TEST_F(DocumentTaskHandlerTest, OverEntCapAsCeLabelsCe) {
    cfg_.Set("f42.async_max_pages", "1000");
    auto r = handler_->SubmitAsync(MakeParams("docHugeCe", 5000, /*ent=*/false));
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(r.body["error"]["structured_data"]["async_overflow"], false);
}

TEST_F(DocumentTaskHandlerTest, ThresholdConfigurable) {
    cfg_.Set("f42.async_threshold_pages", "100");
    // 150 pages > 100 threshold → async overflow path.
    auto r = handler_->SubmitAsync(MakeParams("docT", 150, true));
    EXPECT_EQ(r.status, 202);
}

// ---- GET progress (§6.3) ---------------------------------------------------

TEST_F(DocumentTaskHandlerTest, GetProgressReturnsBodyWithMeta) {
    auto sub = handler_->SubmitAsync(MakeParams("docP", 800, true));
    std::string task_id = sub.body["task_id"];
    // advance some progress
    auto t = mgr_.GetTask(task_id).value();
    mgr_.MarkProcessing(task_id, 2);
    t = mgr_.GetTask(task_id).value();
    t.processed_pages = 327;
    t.total_pages = 1500;
    t.progress_pct = 21.8f;
    t.current_phase = task_phase::kParsing;
    t.worker_id = 2;
    mgr_.UpdateProgress(t);

    auto r = handler_->GetProgress(task_id);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["task_id"], task_id);
    EXPECT_EQ(r.body["processed_pages"], 327);
    EXPECT_EQ(r.body["total_pages"], 1500);
    // §6.3 meta 5 fields present
    ASSERT_TRUE(r.body.contains("meta"));
    const auto& meta = r.body["meta"];
    EXPECT_EQ(meta["worker_id"], 2);
    EXPECT_EQ(meta["current_phase"], "parsing");
    EXPECT_TRUE(meta.contains("retryable_if_fail"));
    EXPECT_EQ(meta["agent_decision_hint"], "poll_again_in_5s");
    EXPECT_TRUE(meta.contains("estimated_finish_at"));
}

TEST_F(DocumentTaskHandlerTest, GetProgressCompletedHint) {
    auto sub = handler_->SubmitAsync(MakeParams("docDone", 800, true));
    std::string task_id = sub.body["task_id"];
    mgr_.MarkCompleted(task_id, "final-doc");
    auto r = handler_->GetProgress(task_id);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["status"], "completed");
    EXPECT_EQ(r.body["doc_id"], "final-doc");
    EXPECT_EQ(r.body["meta"]["agent_decision_hint"], "done_fetch_doc");
}

TEST_F(DocumentTaskHandlerTest, GetProgressUnknownTask404) {
    auto r = handler_->GetProgress("nope");
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_TASK_NOT_FOUND");
    EXPECT_EQ(r.body["error"]["retryable"], false);
}

// ---- DELETE cancel (§4.3) --------------------------------------------------

TEST_F(DocumentTaskHandlerTest, CancelQueuedReturns200Cancelled) {
    auto sub = handler_->SubmitAsync(MakeParams("docCancelQ", 800, true));
    std::string task_id = sub.body["task_id"];
    auto r = handler_->CancelTask(task_id);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["cancel_requested"], true);
    EXPECT_EQ(r.body["current_status"], "cancelled");  // queued → cancelled directly
    EXPECT_EQ(r.body["estimated_cancellation_in_seconds"], 0);
}

TEST_F(DocumentTaskHandlerTest, CancelProcessingReturns200Cancelling) {
    auto sub = handler_->SubmitAsync(MakeParams("docCancelP", 800, true));
    std::string task_id = sub.body["task_id"];
    mgr_.MarkProcessing(task_id, 1);
    auto r = handler_->CancelTask(task_id);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["current_status"], "cancelling");
    EXPECT_EQ(r.body["estimated_cancellation_in_seconds"], 5);
}

TEST_F(DocumentTaskHandlerTest, RepeatCancelReturns423) {
    auto sub = handler_->SubmitAsync(MakeParams("docRepeat", 800, true));
    std::string task_id = sub.body["task_id"];
    mgr_.MarkProcessing(task_id, 1);
    ASSERT_EQ(handler_->CancelTask(task_id).status, 200);  // first → cancelling
    auto r = handler_->CancelTask(task_id);                // second → 423
    EXPECT_EQ(r.status, 423);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_TASK_CANCELLING");
    EXPECT_EQ(r.body["error"]["structured_data"]["current_status"], "cancelling");
}

TEST_F(DocumentTaskHandlerTest, CancelUnknownTask404) {
    auto r = handler_->CancelTask("ghost");
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_TASK_NOT_FOUND");
}

// ---- agent_decision_hint per status (§6.3 BuildProgressBody, all branches) -

TEST(DocumentTaskHandlerBodyTest, AgentDecisionHintCoversAllStatuses) {
    auto hint = [](const std::string& status) {
        TaskInfo t;
        t.task_id = "t1";
        t.status = status;
        return DocumentTaskHandler::BuildProgressBody(t)["meta"]["agent_decision_hint"]
            .get<std::string>();
    };
    EXPECT_EQ(hint(task_status::kQueued), "poll_again_in_5s");
    EXPECT_EQ(hint(task_status::kProcessing), "poll_again_in_5s");
    EXPECT_EQ(hint(task_status::kCompleted), "done_fetch_doc");
    EXPECT_EQ(hint(task_status::kCancelling), "cancel_in_progress_poll");
    EXPECT_EQ(hint(task_status::kCancelled), "cancelled_stop");
    EXPECT_EQ(hint(task_status::kFailed), "inspect_error_code");
}

TEST(DocumentTaskHandlerBodyTest, ProgressBodyNullsEmptyDocAndTrace) {
    TaskInfo t;
    t.task_id = "t1";
    t.status = task_status::kQueued;  // doc_id + trace_id empty
    auto body = DocumentTaskHandler::BuildProgressBody(t);
    EXPECT_TRUE(body["doc_id"].is_null());
    EXPECT_TRUE(body["trace_id"].is_null());
    EXPECT_TRUE(body["meta"]["estimated_finish_at"].is_null());
}

// ---- SubmitAsync enqueue-failure path → 503 -------------------------------

TEST(DocumentTaskHandlerErrorTest, EnqueueFailureMapsToServiceUnavailable) {
    // A TaskManager that was never Init()'d makes every SQL op fail, so the
    // scheduler's Enqueue (→ CreateTask) errors → handler maps to 503.
    TaskManager broken;  // no Init() → null db handle → SQL prepare fails
    InMemoryGlobalConfig cfg;
    TaskScheduler sched(&broken, &cfg);
    DocumentTaskHandler handler(&sched, &broken, nullptr, &cfg);

    SubmitParams p;
    p.namespace_id = "ns1";
    p.filename = "big.pdf";
    p.filepath = "/srv/tmp/big.pdf";
    p.doc_id = "docFail";
    p.content_hash = "h";
    p.page_count = 800;  // over threshold
    p.async_overflow = true;

    auto r = handler.SubmitAsync(p);
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_SERVICE_UNAVAILABLE");
    EXPECT_EQ(r.body["error"]["structured_data"]["component"], "tasks_db");
    EXPECT_EQ(r.body["error"]["retryable"], true);
}

// ---- branch coverage: config fallbacks, pool Notify, cancelling-progress hint -

// A null IGlobalConfig makes both AsyncThresholdPages() and EntMaxPages() fall back
// to their defaults (config_ == nullptr branch). 800 pages > default 200 threshold
// and < default 2000 cap, so an async-overflow large doc still mints an async task.
TEST(DocumentTaskHandlerConfigTest, NullConfigUsesDefaultThresholdAndCap) {
    TaskManager mgr;
    ASSERT_TRUE(mgr.Init(":memory:").ok());
    TaskScheduler sched(&mgr, /*config=*/nullptr);
    DocumentTaskHandler handler(&sched, &mgr, /*pool=*/nullptr, /*config=*/nullptr);

    SubmitParams p;
    p.namespace_id = "ns1";
    p.filename = "big.pdf";
    p.filepath = "/srv/tmp/big.pdf";
    p.doc_id = "docDefault";
    p.content_hash = "h";
    p.page_count = 800;
    p.async_overflow = true;
    auto r = handler.SubmitAsync(p);
    EXPECT_EQ(r.status, 202);  // default threshold 200 < 800 < default cap 2000

    // A doc above the default 2000 cap is rejected for everyone.
    p.page_count = 5000;
    auto over = handler.SubmitAsync(p);
    EXPECT_EQ(over.status, 403);
    EXPECT_EQ(over.body["error"]["structured_data"]["max_pages"], 2000);  // default cap
}

// When a WorkerPool is wired in, the successful enqueue path calls pool_->Notify()
// (the 'if (pool_)' true branch). Notify() only signals a condition variable, so it
// is safe to call without Start()ing any worker threads.
TEST(DocumentTaskHandlerPoolTest, EnqueueNotifiesWorkerPool) {
    TaskManager mgr;
    ASSERT_TRUE(mgr.Init(":memory:").ok());
    InMemoryGlobalConfig cfg;
    TaskScheduler sched(&mgr, &cfg);
    WorkerPool pool(&sched, &cfg);  // not Start()ed; Notify is a no-op signal
    DocumentTaskHandler handler(&sched, &mgr, &pool, &cfg);

    SubmitParams p;
    p.namespace_id = "ns1";
    p.filename = "big.pdf";
    p.filepath = "/srv/tmp/big.pdf";
    p.doc_id = "docPool";
    p.content_hash = "h";
    p.page_count = 800;
    p.async_overflow = true;
    auto r = handler.SubmitAsync(p);
    EXPECT_EQ(r.status, 202);
    EXPECT_EQ(mgr.CountAll().value(), 1);
}

// GetProgress on a cancelling task surfaces the cancel_in_progress_poll hint via
// the live handler path (the dedicated hint test exercises BuildProgressBody only).
TEST_F(DocumentTaskHandlerTest, GetProgressCancellingHint) {
    auto sub = handler_->SubmitAsync(MakeParams("docCanc", 800, true));
    std::string task_id = sub.body["task_id"];
    mgr_.MarkProcessing(task_id, 1);
    ASSERT_EQ(handler_->CancelTask(task_id).status, 200);  // → cancelling
    auto r = handler_->GetProgress(task_id);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["status"], "cancelling");
    EXPECT_EQ(r.body["meta"]["agent_decision_hint"], "cancel_in_progress_poll");
}

// A cancelled (terminal) task surfaces the cancelled_stop hint.
TEST_F(DocumentTaskHandlerTest, GetProgressCancelledHint) {
    auto sub = handler_->SubmitAsync(MakeParams("docCancT", 800, true));
    std::string task_id = sub.body["task_id"];
    ASSERT_EQ(handler_->CancelTask(task_id).status, 200);  // queued → cancelled
    auto r = handler_->GetProgress(task_id);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["status"], "cancelled");
    EXPECT_EQ(r.body["meta"]["agent_decision_hint"], "cancelled_stop");
}

// A failed task surfaces the inspect_error_code hint via the live handler path.
TEST_F(DocumentTaskHandlerTest, GetProgressFailedHint) {
    auto sub = handler_->SubmitAsync(MakeParams("docFail", 800, true));
    std::string task_id = sub.body["task_id"];
    mgr_.MarkFailed(task_id, "CX_ERR_PARSE_FAILED", "boom", "{}");
    auto r = handler_->GetProgress(task_id);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["status"], "failed");
    EXPECT_EQ(r.body["meta"]["agent_decision_hint"], "inspect_error_code");
}

// ---- Issue 5 GEN-Agent 4-field schema across all 11 codes -------------------

TEST(DocumentTaskHandlerSchemaTest, AllElevenCodesEmitFourFieldEnvelope) {
    // Every CX_ERR_* serialized via ToJson(MakeTaskError(...)) carries the 4
    // GEN-Agent fields with the correct types (§6.2 / AGENT_FRIENDLY §3.1).
    constexpr TaskErrorCode kAll[] = {
        TaskErrorCode::kInvalidRequest,          TaskErrorCode::kMaxPagesExceeded,
        TaskErrorCode::kTaskNotFound,            TaskErrorCode::kTaskTimeout,
        TaskErrorCode::kDocProcessingInProgress, TaskErrorCode::kTaskCancelling,
        TaskErrorCode::kParseFailed,             TaskErrorCode::kStorageFailed,
        TaskErrorCode::kWorkerBusy,              TaskErrorCode::kZombieTaskCleanup,
        TaskErrorCode::kServiceUnavailable,
    };
    std::set<std::string> codes;
    for (TaskErrorCode c : kAll) {
        nlohmann::json j = agent_friendly::ToJson(MakeTaskError(c));
        ASSERT_TRUE(j.contains("code"));
        ASSERT_TRUE(j.contains("retryable"));
        ASSERT_TRUE(j.contains("category"));
        ASSERT_TRUE(j.contains("retry_after_ms"));     // null or int
        ASSERT_TRUE(j.contains("structured_data"));
        EXPECT_TRUE(j["retryable"].is_boolean());
        EXPECT_TRUE(j["category"].is_string());
        // retryable ⇒ retry_after_ms is a positive int; else null (GEN-Agent #6).
        if (j["retryable"].get<bool>()) {
            EXPECT_TRUE(j["retry_after_ms"].is_number_integer());
            EXPECT_GT(j["retry_after_ms"].get<int>(), 0);
        } else {
            EXPECT_TRUE(j["retry_after_ms"].is_null());
        }
        codes.insert(j["code"].get<std::string>());
    }
    EXPECT_EQ(codes.size(), 11u);  // all distinct
}

}  // namespace
}  // namespace cortrix::async
