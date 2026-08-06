#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <thread>

#include "cortrix/async/document_processor.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/async/worker_pool.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_factory.h"
#include "parser_stub.h"
#include "test_name_util.h"

// S2 coverage: WorkerPool — Issue 1.1 pool sizing from config, Issue 1.2 startup
// enforce (pool_size <= parser_max_concurrent), and the end-to-end Dequeue →
// ProcessTask → OnTaskCompleted drain loop over a real (in-memory) stack.
namespace cortrix::async {
namespace {

using spc::test::MakeOnePageDoc;
using spc::test::StubParser;

std::unique_ptr<StubParser> MakeOkStub() {
    auto stub = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    stub->SetResult(MakeOnePageDoc("docling", 0.9f));
    stub->SetOnParse([](const std::string&, const spc::ParserOptions& opts) {
        if (opts.on_page_progress) opts.on_page_progress(1, 1, true);
    });
    return stub;
}

class WorkerPoolTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        sched_ = std::make_unique<TaskScheduler>(&mgr_, &cfg_);
        factory_ = std::make_unique<spc::DocumentParserFactory>(fcfg_);
        factory_->SetPrimaryParser(MakeOkStub());
        proc_ = std::make_unique<DocumentProcessor>(&mgr_, factory_.get(), &cfg_);
        // Parser factory pre-check stat()s the file → seed a real temp .pdf.
        // Unique per test: parallel ctest processes must not share the stub
        // (a sibling's TearDown/remove yanks it mid-parse; F-1 race family).
        filepath_ = std::string(::testing::TempDir()) + "async_worker_test_" +
                    cortrix::test::SanitizedTestName() +
                    ".pdf";
        std::ofstream(filepath_) << "%PDF-1.4 stub";
    }
    void TearDown() override { std::remove(filepath_.c_str()); }

    SubmitRequest MakeReq(const std::string& doc) {
        SubmitRequest r;
        r.namespace_id = "ns1";
        r.filename = "f.pdf";
        r.filepath = filepath_;
        r.doc_id = doc;
        r.content_hash = doc + "-h";
        return r;
    }

    // Wait until every task in the namespace is terminal, or timeout.
    bool WaitAllTerminal(int n, int timeout_ms = 3000) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            auto list = mgr_.ListByNamespace("ns1", 100, 0);
            int terminal = 0;
            for (const auto& t : list.value()) {
                if (t.status == task_status::kCompleted ||
                    t.status == task_status::kFailed ||
                    t.status == task_status::kCancelled) {
                    ++terminal;
                }
            }
            if (terminal >= n) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    TaskManager mgr_;
    InMemoryGlobalConfig cfg_;
    spc::ParserFactoryConfig fcfg_;
    std::unique_ptr<TaskScheduler> sched_;
    std::unique_ptr<spc::DocumentParserFactory> factory_;
    std::unique_ptr<DocumentProcessor> proc_;
    std::string filepath_;
};

// ---- Issue 1.1 sizing -------------------------------------------------------

TEST_F(WorkerPoolTest, DefaultPoolSizeIsTwo) {
    WorkerPool pool(sched_.get(), nullptr);  // no config → defaults
    EXPECT_EQ(pool.ConfiguredPoolSize(), 2);
    EXPECT_EQ(pool.ParserMaxConcurrent(), WorkerPool::kDefaultParserMaxConcurrent);
}

TEST_F(WorkerPoolTest, PoolSizeReadFromConfig) {
    cfg_.Set("async.worker_pool_size", "3");
    cfg_.Set("parser.parser_max_concurrent", "8");
    WorkerPool pool(sched_.get(), &cfg_);
    EXPECT_EQ(pool.ConfiguredPoolSize(), 3);
    EXPECT_EQ(pool.ParserMaxConcurrent(), 8);
}

// ---- Issue 1.2 startup enforce ----------------------------------------------

TEST_F(WorkerPoolTest, StartSucceedsWhenWithinParserCap) {
    cfg_.Set("async.worker_pool_size", "4");
    cfg_.Set("parser.parser_max_concurrent", "4");  // equal is allowed
    WorkerPool pool(sched_.get(), &cfg_);
    Status s = pool.Start();
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(pool.worker_count(), 4);
    pool.Stop();
}

TEST_F(WorkerPoolTest, StartFailsWhenExceedingParserCap) {
    cfg_.Set("async.worker_pool_size", "5");
    cfg_.Set("parser.parser_max_concurrent", "2");  // 5 > 2 → fatal misconfig
    WorkerPool pool(sched_.get(), &cfg_);
    Status s = pool.Start();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_INVALID_REQUEST"), std::string::npos);
    EXPECT_EQ(pool.worker_count(), 0);  // no threads started
}

TEST_F(WorkerPoolTest, StartFailsWhenPoolSizeZero) {
    cfg_.Set("async.worker_pool_size", "0");
    cfg_.Set("parser.parser_max_concurrent", "4");
    WorkerPool pool(sched_.get(), &cfg_);
    EXPECT_FALSE(pool.Start().ok());
}

// ---- end-to-end drain ------------------------------------------------------

TEST_F(WorkerPoolTest, DrainsQueuedTasksToCompletion) {
    cfg_.Set("async.worker_pool_size", "2");
    cfg_.Set("parser.parser_max_concurrent", "4");
    WorkerPool pool(sched_.get(), &cfg_);
    pool.RegisterHandler(kTaskDocParse, proc_.get());  // route doc-parse tasks
    ASSERT_TRUE(pool.Start().ok());

    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(sched_->Enqueue(MakeReq("doc" + std::to_string(i))).ok());
        pool.Notify();
    }
    EXPECT_TRUE(WaitAllTerminal(6));
    pool.Stop();

    auto list = mgr_.ListByNamespace("ns1", 100, 0);
    int completed = 0;
    for (const auto& t : list.value())
        if (t.status == task_status::kCompleted) ++completed;
    EXPECT_EQ(completed, 6);
    EXPECT_EQ(sched_->ActiveDocCount(), 0u);  // all doc_ids released
}

TEST_F(WorkerPoolTest, StopIsIdempotentAndSafeWithoutStart) {
    WorkerPool pool(sched_.get(), &cfg_);
    pool.Stop();  // never started → no-op
    cfg_.Set("parser.parser_max_concurrent", "4");
    ASSERT_TRUE(pool.Start().ok());
    pool.Stop();
    pool.Stop();  // double stop safe
    SUCCEED();
}

}  // namespace
}  // namespace cortrix::async
