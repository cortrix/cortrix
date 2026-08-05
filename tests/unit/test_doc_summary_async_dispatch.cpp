// doc_summary end-to-end dispatch (candidate ① D3.5 within-feature E2E): proves
// the data path wired by ⑤a (SqliteChunkStore) + ⑤b (F41AsyncWorker) + T4 (WorkerPool
// task_type dispatch) + the main wiring actually runs THROUGH the real async task scheduler:
//
//   TaskScheduler::Enqueue(kTaskDocSummary)
//     → WorkerPool worker thread Dequeue
//       → dispatch by task_type to the registered F41AsyncWorker
//         → SqliteChunkStore + DocSummaryGenerator (MockLlmClient) + OnnxEmbedder stub
//           → doc_summary Block (block_type=17) + P-HNSW point via the write coordinator PWL.
//
// Unlike test_f41_async_worker.cpp (⑤b) which calls worker.ProcessTask() directly, this
// drives the worker through real WorkerPool threads; unlike test_worker_pool_dispatch.cpp
// (T4) which routes to recording doubles, this routes to the REAL F41AsyncWorker and
// asserts the block actually lands. It mirrors main.cpp's "7b" topology (one WorkerPool,
// kTaskDocParse + kTaskDocSummary handlers coexisting).
//
// Synchronization: the worker is wrapped in a SignalingWorker that notifies on
// completion, so the test awaits the worker finishing (façade already Released) and reads
// the per-Unit store only AFTER pool.Stop() — never polling the SQLite Unit store while a
// worker thread writes it (that contends on the WAL lock; a harness artifact, not a
// product issue).
//
// finalize ownership = handler (async task · decision A, 2026-06-09): F41AsyncWorker
// finalizes its own task.status via TaskFinalizer (success → completed, failure → failed);
// retry/DLQ is the Phase-2 framework layer above finalize (a separate retry/DLQ item). So this
// test now asserts BOTH the block landing AND the terminal task.status=completed.
#include "cortrix/doc_summary/f41_async_worker.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/async/task_handler.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/async/task_type.h"
#include "cortrix/async/worker_pool.h"
#include "cortrix/chunker/types.h"          // chunker::ChildChunk
#include "cortrix/common/block_types.h"     // kBlockDocSummary / kBlockFile
#include "cortrix/common/data_types.h"      // CortrixDoc / CortrixBlock
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/doc_summary/doc_summary_metrics.h"  // SummariesGeneratedCount (recorded by the generator)
#include "cortrix/id/hash.h"                // SetDeploymentHashKeyForTesting
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store.h"

#include "mock_llm_client.h"
#include "ns_pool_test_helper.h"

namespace cortrix::doc_summary {
namespace {

using ::testing::_;
using ::testing::NiceMock;

llm::ChatCompletionResponse OkChat(std::string content) {
    llm::ChatCompletionResponse r;
    r.content = std::move(content);
    r.model = "gpt-4o-mini";
    r.prompt_tokens = 100;
    r.completion_tokens = 60;
    return r;  // Status defaults ok()
}

// A valid §9.1 structured-output body the DocSummaryGenerator will parse.
const char* kSummaryJson = R"({
  "summary_text": "Q3 2026 financials: revenue grew twenty three percent year over year.",
  "keywords": ["revenue", "finance", "Q3"],
  "topics": ["Financial Report"],
  "one_liner": "Q3 2026 financials"
})";

// Minimal recording handler occupying kTaskDocParse, to assert routing isolation:
// a doc-parse task must reach THIS handler, not the doc_summary worker.
class RecordingHandler : public async::ITaskHandler {
public:
    Status ProcessTask(const async::TaskInfo& task) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            seen_.push_back(task.task_type);
        }
        cv_.notify_all();
        return Status::Ok();
    }
    bool WaitFor(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return seen_.size() >= count; });
    }
    std::vector<int> seen() {
        std::lock_guard<std::mutex> lk(mu_);
        return seen_;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<int> seen_;
};

// Wraps the real F41AsyncWorker so the test can await its completion on a pool thread
// without polling the per-Unit store concurrently. ProcessTask delegates to the inner
// worker (which Acquires + Releases its own façade), then notifies — so when WaitFor
// returns, the worker's write txn is committed and its façade Released.
class SignalingWorker : public async::ITaskHandler {
public:
    explicit SignalingWorker(F41AsyncWorker* inner) : inner_(inner) {}
    Status ProcessTask(const async::TaskInfo& task) override {
        Status s = inner_->ProcessTask(task);
        {
            std::lock_guard<std::mutex> lk(mu_);
            last_status_ = s;
            ++done_;
        }
        cv_.notify_all();
        return s;
    }
    bool WaitFor(int count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return done_ >= count; });
    }
    Status last_status() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_status_;
    }

private:
    F41AsyncWorker* inner_;
    std::mutex mu_;
    std::condition_variable cv_;
    int done_ = 0;
    Status last_status_ = Status::Ok();
};

class DocSummaryAsyncDispatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Stable, non-zero block ids from HashChildIdToBlockId (assembler).
        id::SetDeploymentHashKeyForTesting({0x0123456789abcdefULL, 0xfedcba9876543210ULL});
        DocSummaryMetrics::Instance().ResetForTest();

        harness_ = std::make_unique<test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("f41_dispatch_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        ASSERT_TRUE(harness_->Admit("test-ns").ok());

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();

        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        cfg_.Set("f42.worker_pool_size", "2");
        cfg_.Set("f06.parser_max_concurrent", "4");
        sched_ = std::make_unique<async::TaskScheduler>(&mgr_, &cfg_);
    }

    void TearDown() override { harness_.reset(); }

    // Seed a doc + `n_chunks` child chunks on the per-Unit store via a scoped façade
    // (Acquired then Released here) — identical to test_f41_async_worker.cpp. Returns
    // the doc's ULID. The worker later self-Acquires the same namespace.
    std::string SeedDoc(int n_chunks) {
        resource::NamespaceFacade f(harness_->ipool(), "test-ns");
        EXPECT_TRUE(f.Acquire().ok());
        CortrixDoc doc;
        doc.source_type = "test";
        doc.source_path = "/f41/doc.txt";
        EXPECT_EQ(f.store().doc_create(doc), 0);
        BlockAssembler assembler;
        for (int i = 0; i < n_chunks; ++i) {
            chunker::ChildChunk c;
            c.child_id = "ch-" + doc.doc_id + "-" + std::to_string(i);
            c.parent_id = "par-" + doc.doc_id;
            c.doc_id = doc.doc_id;
            c.chunk_index = i;
            c.child_text = "chunk body number " + std::to_string(i);
            EmbeddingResult emb;  // child embedding irrelevant to the summary path
            CortrixBlock b = assembler.AssembleChild(c, emb, kBlockFile, 3, "");
            EXPECT_EQ(f.store().block_insert(b), 0);
        }
        return doc.doc_id;
    }

    // Read back the doc's blocks through a scoped façade. Call only AFTER the pool is
    // stopped, so no worker thread is writing the same Unit store concurrently.
    std::vector<CortrixBlock> BlocksOf(const std::string& doc_id) {
        resource::NamespaceFacade f(harness_->ipool(), "test-ns");
        EXPECT_TRUE(f.Acquire().ok());
        std::vector<CortrixBlock> out;
        EXPECT_EQ(f.store().block_get_by_doc(doc_id, out), 0);
        return out;
    }

    std::optional<CortrixBlock> DocSummaryBlockOf(const std::string& doc_id) {
        for (auto& b : BlocksOf(doc_id)) {
            if (b.block_type == static_cast<int>(kBlockDocSummary)) return b;
        }
        return std::nullopt;
    }

    std::shared_ptr<NiceMock<llm::MockLlmClient>> MakeLlm(const char* json) {
        auto m = std::make_shared<NiceMock<llm::MockLlmClient>>();
        ON_CALL(*m, Chat(_, _)).WillByDefault([json](const std::string&, const llm::LlmCallConfig&) {
            return OkChat(json);
        });
        return m;
    }

    F41AsyncWorker MakeWorker(std::shared_ptr<llm::ILlmClient> llm) {
        return F41AsyncWorker(harness_->ipool(), std::move(llm), DocSummaryConfig{},
                              *embedder_, assembler_, &mgr_);
    }

    std::string Enqueue(int task_type, const std::string& doc_id,
                        const std::string& ns = "test-ns") {
        async::SubmitRequest req;
        req.namespace_id = ns;
        req.filename = "f.bin";
        req.filepath = "/tmp/" + doc_id;
        req.doc_id = doc_id;
        req.content_hash = doc_id;  // distinct per doc → no debounce collisions
        req.total_pages = 1;
        req.task_type = task_type;
        auto r = sched_->Enqueue(req);
        EXPECT_TRUE(r.ok());
        return r.ok() ? r.value().task_id : std::string{};
    }

    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    BlockAssembler assembler_;
    async::TaskManager mgr_;
    InMemoryGlobalConfig cfg_;
    std::unique_ptr<async::TaskScheduler> sched_;
};

// E2E: a kTaskDocSummary submitted to the scheduler is dispatched by a real WorkerPool
// thread to the real F41AsyncWorker, which lands the doc_summary block + P-HNSW point.
TEST_F(DocSummaryAsyncDispatchTest, DocSummaryTaskDispatchedThroughPoolWritesBlock) {
    const std::string doc_id = SeedDoc(3);
    F41AsyncWorker worker = MakeWorker(MakeLlm(kSummaryJson));
    SignalingWorker signaling(&worker);

    async::WorkerPool pool(sched_.get(), &cfg_);
    pool.RegisterHandler(async::kTaskDocSummary, &signaling);
    ASSERT_TRUE(pool.Start().ok());

    const std::string task_id = Enqueue(async::kTaskDocSummary, doc_id);
    pool.Notify();

    ASSERT_TRUE(signaling.WaitFor(1, std::chrono::seconds(3)));
    pool.Stop();  // join workers before reading the store/FakeIndex (no read/write race)

    EXPECT_TRUE(signaling.last_status().ok()) << signaling.last_status().message();
    // finalize ownership = handler (async task · decision A): the worker finalizes its
    // own task.status via TaskFinalizer → completed (was the KNOWN GAP).
    auto ft = mgr_.GetTask(task_id);
    ASSERT_TRUE(ft.ok());
    EXPECT_EQ(ft.value().status, async::task_status::kCompleted);

    auto block = DocSummaryBlockOf(doc_id);
    ASSERT_TRUE(block.has_value());
    EXPECT_NE(block->content_text.find("Q3 2026 financials"), std::string::npos);

    auto meta = nlohmann::json::parse(block->metadata_json, nullptr, false);
    ASSERT_FALSE(meta.is_discarded());
    EXPECT_EQ(meta.value("status", ""), "generated");
    EXPECT_EQ(meta["keywords"].size(), 3u);
    EXPECT_EQ(meta.value("one_liner", ""), "Q3 2026 financials");

    // The summary embedding reached the P-HNSW index via the write coordinator PWL on the pool thread.
    EXPECT_FALSE(harness_->fake_index()->added_ids().empty());
    EXPECT_EQ(DocSummaryMetrics::Instance().SummariesGeneratedCount(), 1u);
}

// E2E: kTaskDocParse + kTaskDocSummary handlers coexist on one pool (main.cpp 7b topology);
// each task_type routes to its own handler — the parse task never reaches the summary
// worker, and the summary task lands its block.
TEST_F(DocSummaryAsyncDispatchTest, CoexistingHandlersRouteByTaskType) {
    const std::string sum_doc = SeedDoc(2);
    F41AsyncWorker worker = MakeWorker(MakeLlm(kSummaryJson));
    SignalingWorker signaling(&worker);
    RecordingHandler parse_h;

    async::WorkerPool pool(sched_.get(), &cfg_);
    pool.RegisterHandler(async::kTaskDocParse, &parse_h);
    pool.RegisterHandler(async::kTaskDocSummary, &signaling);
    ASSERT_TRUE(pool.Start().ok());

    Enqueue(async::kTaskDocParse, "parse-doc-1");  // distinct doc_id from the summary task
    Enqueue(async::kTaskDocSummary, sum_doc);
    pool.Notify();
    pool.Notify();

    ASSERT_TRUE(parse_h.WaitFor(1, std::chrono::seconds(2)));
    ASSERT_TRUE(signaling.WaitFor(1, std::chrono::seconds(3)));
    pool.Stop();

    // Routing isolation: the parse handler saw only the parse task; the summary worker
    // landed a block for its doc and wrote nothing for the parse doc.
    EXPECT_EQ(parse_h.seen(), (std::vector<int>{async::kTaskDocParse}));
    EXPECT_TRUE(signaling.last_status().ok()) << signaling.last_status().message();
    EXPECT_TRUE(DocSummaryBlockOf(sum_doc).has_value());
    EXPECT_FALSE(DocSummaryBlockOf("parse-doc-1").has_value());
}

// E2E (Fail path, symmetric to Test1): a doc_summary task whose generation fails (invalid
// LLM output) routes through the pool to the worker, which finalizes the task as FAILED
// with no block written (handler-owned finalize · async task). Proves the failure half of
// the closed finalize loop — the task reaches a terminal state, not stuck "processing".
TEST_F(DocSummaryAsyncDispatchTest, GenerationFailureFinalizesTaskFailed) {
    const std::string doc_id = SeedDoc(2);
    F41AsyncWorker worker = MakeWorker(MakeLlm("not valid json"));  // generation fails
    SignalingWorker signaling(&worker);

    async::WorkerPool pool(sched_.get(), &cfg_);
    pool.RegisterHandler(async::kTaskDocSummary, &signaling);
    ASSERT_TRUE(pool.Start().ok());

    const std::string task_id = Enqueue(async::kTaskDocSummary, doc_id);
    pool.Notify();

    ASSERT_TRUE(signaling.WaitFor(1, std::chrono::seconds(3)));
    pool.Stop();

    // Worker returned a failure Status; the task was finalized as failed (terminal, not
    // stuck), carrying a CX_ERR_DOCSUMMARY_* code, and no doc_summary block landed (doc-discovery
    // degrades to the FTS5 fallback for this doc, doc summary).
    EXPECT_FALSE(signaling.last_status().ok());
    auto ft = mgr_.GetTask(task_id);
    ASSERT_TRUE(ft.ok());
    EXPECT_EQ(ft.value().status, async::task_status::kFailed);
    EXPECT_FALSE(ft.value().error_code.empty());
    EXPECT_FALSE(DocSummaryBlockOf(doc_id).has_value());
}

}  // namespace
}  // namespace cortrix::doc_summary
