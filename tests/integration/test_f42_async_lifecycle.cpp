#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "cortrix/async/document_processor.h"
#include "cortrix/async/document_task_handler.h"
#include "cortrix/async/task_cleanup_cron.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/async/worker_pool.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_factory.h"
#include "parser_stub.h"

// F42 standalone integration (within-feature E2E): the full async document
// lifecycle wired end to end — HTTP handler → TaskScheduler → WorkerPool →
// DocumentProcessor → F06 (stub) parser → progress/cancel/cleanup — over a real
// in-memory stack. The CROSS-feature E2E (real Docling subprocess, real SPC
// Enricher/Chunk/Index, real F25 write + RollbackCallback, real server routes)
// is D3.5 deferred wiring; here the only mock is the injected stub IDocumentParser.
namespace cortrix::async {
namespace {

using spc::test::MakeOnePageDoc;
using spc::test::StubParser;

// A stub parser that drives `pages` on_page_progress callbacks; an optional
// per-page hook lets a test observe/influence progress (e.g. trigger a cancel).
std::unique_ptr<StubParser> MakeStreamingStub(
    int pages,
    std::function<void(int page)> per_page = nullptr) {
    auto stub = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    stub->SetResult(MakeOnePageDoc("docling", 0.95f));
    stub->SetOnParse([pages, per_page](const std::string&, const spc::ParserOptions& opts) {
        for (int p = 1; p <= pages; ++p) {
            if (per_page) per_page(p);
            if (opts.on_page_progress) opts.on_page_progress(p, pages, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    return stub;
}

class F42LifecycleTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        cfg_.Set("f42.worker_pool_size", "2");
        cfg_.Set("f06.parser_max_concurrent", "4");
        cfg_.Set("f42.async_threshold_pages", "200");
        cfg_.Set("f42.async_max_pages", "2000");
        sched_ = std::make_unique<TaskScheduler>(&mgr_, &cfg_);
        factory_ = std::make_unique<spc::DocumentParserFactory>(fcfg_);
        proc_ = std::make_unique<DocumentProcessor>(&mgr_, factory_.get(), &cfg_);
        pool_ = std::make_unique<WorkerPool>(sched_.get(), &cfg_);
        pool_->RegisterHandler(kTaskDocParse, proc_.get());
        handler_ = std::make_unique<DocumentTaskHandler>(sched_.get(), &mgr_,
                                                        pool_.get(), &cfg_);
        filepath_ = std::string(::testing::TempDir()) + "f42_lifecycle.pdf";
        std::ofstream(filepath_) << "%PDF-1.4 stub";
    }
    void TearDown() override {
        pool_->Stop();
        std::remove(filepath_.c_str());
    }

    SubmitParams MakeParams(const std::string& doc, int pages, bool ent = true) {
        SubmitParams p;
        p.namespace_id = "ns1";
        p.filename = "big.pdf";
        p.filepath = filepath_;
        p.doc_id = doc;
        p.content_hash = doc + "-h";
        p.page_count = pages;
        p.async_overflow = ent;
        return p;
    }

    bool WaitStatus(const std::string& task_id, const std::string& want,
                    int timeout_ms = 3000) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            auto t = mgr_.GetTask(task_id);
            if (t.ok() && t.value().status == want) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    TaskManager mgr_;
    InMemoryGlobalConfig cfg_;
    spc::ParserFactoryConfig fcfg_;
    std::unique_ptr<TaskScheduler> sched_;
    std::unique_ptr<spc::DocumentParserFactory> factory_;
    std::unique_ptr<DocumentProcessor> proc_;
    std::unique_ptr<WorkerPool> pool_;
    std::unique_ptr<DocumentTaskHandler> handler_;
    std::string filepath_;
};

// E2E #1: POST async → worker picks it up → parses → GET shows completed.
TEST_F(F42LifecycleTest, SubmitProcessCompleteViaPolling) {
    factory_->SetPrimaryParser(MakeStreamingStub(8));
    ASSERT_TRUE(pool_->Start().ok());

    auto sub = handler_->SubmitAsync(MakeParams("docE2E", 800));
    ASSERT_EQ(sub.status, 202);
    std::string task_id = sub.body["task_id"];

    ASSERT_TRUE(WaitStatus(task_id, task_status::kCompleted));
    auto prog = handler_->GetProgress(task_id);
    EXPECT_EQ(prog.status, 200);
    EXPECT_EQ(prog.body["status"], "completed");
    EXPECT_EQ(prog.body["meta"]["agent_decision_hint"], "done_fetch_doc");
    EXPECT_EQ(sched_->ActiveDocCount(), 0u);  // doc_id released
}

// E2E #2: cancel a processing task mid-stream → worker stops at checkpoint →
// GET reflects cancelled (Issue 3.1 chunk-index checkpoint).
TEST_F(F42LifecycleTest, CancelMidProcessingStopsAtCheckpoint) {
    std::string task_id;
    // The per-page hook fires the DELETE cancel once we are clearly mid-stream.
    auto stub = MakeStreamingStub(40, [&](int page) {
        if (page == 5 && !task_id.empty()) handler_->CancelTask(task_id);
    });
    factory_->SetPrimaryParser(std::move(stub));
    ASSERT_TRUE(pool_->Start().ok());

    auto sub = handler_->SubmitAsync(MakeParams("docCancelE2E", 800));
    ASSERT_EQ(sub.status, 202);
    task_id = sub.body["task_id"];

    ASSERT_TRUE(WaitStatus(task_id, task_status::kCancelled));
    auto prog = handler_->GetProgress(task_id);
    EXPECT_EQ(prog.body["status"], "cancelled");
    // It stopped early: fewer than all 40 pages were processed before cancel.
    EXPECT_LT(prog.body["processed_pages"].get<int>(), 40);
    EXPECT_EQ(sched_->ActiveDocCount(), 0u);
}

// E2E #3: same-doc_id submitted twice (debounce off) → per-doc_id mutex serializes
// them; both ultimately complete, never concurrently active.
TEST_F(F42LifecycleTest, SameDocSerializedThenBothComplete) {
    cfg_.Set("f42.watcher_debounce_seconds", "0");  // force two distinct rows
    factory_->SetPrimaryParser(MakeStreamingStub(6));
    ASSERT_TRUE(pool_->Start().ok());

    auto a = handler_->SubmitAsync(MakeParams("docSame", 800));
    auto b = handler_->SubmitAsync(MakeParams("docSame", 800));
    ASSERT_EQ(a.status, 202);
    ASSERT_EQ(b.status, 202);
    ASSERT_NE(a.body["task_id"], b.body["task_id"]);

    EXPECT_TRUE(WaitStatus(a.body["task_id"], task_status::kCompleted));
    EXPECT_TRUE(WaitStatus(b.body["task_id"], task_status::kCompleted));
    EXPECT_EQ(sched_->ActiveDocCount(), 0u);
}

// E2E #4: a crashed-worker zombie (processing, stale) is swept to failed by the
// cleanup cron, then GET surfaces the GEN-Agent zombie error code.
TEST_F(F42LifecycleTest, ZombieSweptByCronThenVisibleViaProgress) {
    // Seed a processing task directly (simulating a worker that died), then run
    // the cron with a 0h zombie threshold after letting time advance.
    auto sub = handler_->SubmitAsync(MakeParams("docZombie", 800));
    std::string task_id = sub.body["task_id"];
    ASSERT_TRUE(mgr_.MarkProcessing(task_id, 1).ok());

    cfg_.Set("f42.zombie_task_threshold_hours", "0");
    cfg_.Set("f42.task_retention_days", "9999");  // don't delete, just flip to failed
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    TaskCleanupCron cron(&mgr_, &cfg_);
    int deleted = 0, zombies = 0;
    cron.RunCleanupNow(&deleted, &zombies);
    EXPECT_EQ(zombies, 1);

    auto prog = handler_->GetProgress(task_id);
    EXPECT_EQ(prog.body["status"], "failed");
    auto got = mgr_.GetTask(task_id);
    EXPECT_EQ(got.value().error_code, "CX_ERR_ZOMBIE_TASK_CLEANUP");
}

// E2E #5: Watcher debounce merge across the handler — a re-submit of the same
// doc_id+content_hash inside the window returns the same task_id, no 2nd row.
TEST_F(F42LifecycleTest, WatcherDebounceMergeThroughHandler) {
    // pool not started → tasks stay queued so the window check is deterministic.
    auto first = handler_->SubmitAsync(MakeParams("docDebounce", 800));
    auto dup = handler_->SubmitAsync(MakeParams("docDebounce", 800));
    ASSERT_EQ(first.status, 202);
    ASSERT_EQ(dup.status, 202);
    EXPECT_EQ(first.body["task_id"], dup.body["task_id"]);  // merged
    EXPECT_EQ(mgr_.CountAll().value(), 1);
}

}  // namespace
}  // namespace cortrix::async
