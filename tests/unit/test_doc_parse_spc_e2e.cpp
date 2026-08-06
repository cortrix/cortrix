// doc-parse → SPC end-to-end (B3 · Plan B · async task): proves the async large-doc
// path wired by B1 (SPCPipeline::ProcessParsed split) + B2a (SPCManager::ProcessParsedDoc
// proxy) + B2b-1 (DocumentProcessor hands its parsed doc to SPC) actually runs THROUGH
// the REAL SPC pipeline and lands blocks — not just the call contract a mock verifies:
//
//   DocumentProcessor::ProcessTask
//     → parse (StubParser drives per-page progress, returns a ParsedDoc)
//       → SPCManager::ProcessParsedDoc(parsed, spc_task)   [real, not MockSPCManager]
//         → NamespaceFacade::Acquire (real namespace pool)
//           → SPCPipeline::ProcessParsed (Chunk → META → enrich → embed →
//             assemble → write) → real SQLite `blocks` + P-HNSW point
//     → TaskFinalizer::Complete → task.status = completed
//
// Unlike test_document_processor.cpp's WiredToSpc tests (which inject a MockSPCManager
// returning 0/-1 to verify the *call + finalize* contract), this wires the REAL
// SPCManager + SPCPipeline over an NsPoolHarness (the same proven namespace pool the spc
// pilot stands up) and asserts blocks actually persist. It closes the B3 gap: "real
// ProcessParsed writes blocks", which the mock path could never exercise.
//
// ProcessParsedDoc is a synchronous post-parse entry (not the SPCManager worker queue),
// so DocumentProcessor::ProcessTask runs the whole chain on the test thread — no worker
// thread writes the Unit store concurrently, so reads need no WAL-lock dance.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/async/document_processor.h"
#include "cortrix/async/task_handler.h"         // ITaskHandler (SignalingHandler base)
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"       // real async task scheduler (async dispatch)
#include "cortrix/async/task_type.h"            // kTaskDocParse
#include "cortrix/async/worker_pool.h"          // real WorkerPool (task_type dispatch)
#include "cortrix/chunker/parent_child_chunker.h"
#include "cortrix/common/block_types.h"        // kBlockFile / kBlockMeta
#include "cortrix/common/data_types.h"         // CortrixDoc / CortrixBlock
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/id/hash.h"                    // SetDeploymentHashKeyForTesting
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/parser.h"                 // ParserOptions / on_page_progress
#include "cortrix/spc/parser_factory.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/spc_enricher.h"               // NullEnricher (IsAvailable()==false)
#include "cortrix/store/cortrix_store.h"

#include "ns_pool_test_helper.h"               // test::NsPoolHarness (namespace pool + FakeIndex)
#include "parser_stub.h"                        // spc::test::StubParser / MakeOnePageDoc
#include "test_name_util.h"

namespace cortrix {
namespace {

using spc::ParsedDoc;
using spc::ParserOptions;
using spc::test::MakeOnePageDoc;
using spc::test::StubParser;

// A multi-paragraph body long enough to chunk into several children under the default
// ChunkerConfig (child_size=200), so the write produces real child blocks (+ one META).
std::string LongBody() {
    std::string s;
    for (int i = 0; i < 12; ++i) s += "Cortrix async ingest writes real blocks end to end. ";
    return s;  // ~600 chars
}

// A StubParser that drives opts.on_page_progress for pages 1..total (mirroring parser's
// per-page streaming) then returns `result` — same shape as test_document_processor.cpp.
std::unique_ptr<StubParser> MakeDrivingStub(int total_pages, ParsedDoc result) {
    auto stub = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    stub->SetResult(std::move(result));
    stub->SetOnParse([total_pages](const std::string&, const ParserOptions& opts) {
        if (!opts.on_page_progress) return;
        for (int p = 1; p <= total_pages; ++p) opts.on_page_progress(p, total_pages, true);
    });
    return stub;
}

// Wraps any ITaskHandler so the test can await its completion on a WorkerPool thread
// without polling the per-Unit store concurrently. ProcessTask delegates to inner_ (which
// runs the full parse → SPC write txn), then notifies — so when WaitFor returns the write
// is committed and the façade Released; the store is read only AFTER pool.Stop().
class SignalingHandler : public async::ITaskHandler {
public:
    explicit SignalingHandler(async::ITaskHandler* inner) : inner_(inner) {}
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
    async::ITaskHandler* inner_;
    std::mutex mu_;
    std::condition_variable cv_;
    int done_ = 0;
    Status last_status_ = Status::Ok();
};

class DocParseSpcE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        // Stable, non-zero block ids from HashChildIdToBlockId (assembler).
        id::SetDeploymentHashKeyForTesting({0x0123456789abcdefULL, 0xfedcba9876543210ULL});

        harness_ = std::make_unique<test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("doc_parse_spc_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        ASSERT_TRUE(harness_->Admit("test-ns").ok());

        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        cfg_.Set("async.worker_pool_size", "2");      // for the async-dispatch test (Test3)
        cfg_.Set("parser.parser_max_concurrent", "4");

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        chunker_ = std::make_unique<chunker::ParentChildChunker>(chunker::ChunkerConfig{});
        assembler_ = std::make_unique<BlockAssembler>();

        // Parser factory with the driving stub (its ParsedDoc is what SPC processes).
        factory_ = std::make_unique<spc::DocumentParserFactory>(factory_cfg_);
        factory_->SetPrimaryParser(MakeDrivingStub(2, MakeOnePageDoc("docling", 0.95f, LongBody())));

        // Real SPCPipeline + SPCManager over the harness pool (NOT a mock). The manager
        // is NOT Start()ed: ProcessParsedDoc is the synchronous post-parse entry.
        auto pipeline = std::make_unique<SPCPipeline>(
            *factory_, *chunker_, *embedder_, *assembler_, enricher_);
        spc_mgr_ = std::make_unique<SPCManager>(spc_config_, harness_->ipool(), std::move(pipeline));

        // A real on-disk .pdf so the parser factory pre-check stat() passes (the stub does
        // not read it — it returns the seeded ParsedDoc).
        // Unique per test: parallel ctest processes must not share the stub
        // (a sibling's TearDown/remove yanks it mid-parse; F-1 race family).
        filepath_ =
            (std::filesystem::temp_directory_path() /
             (std::string("b3_doc_parse_") +
              cortrix::test::SanitizedTestName() +
              ".pdf"))
                .string();
        std::ofstream(filepath_) << "%PDF-1.4 stub";
    }

    void TearDown() override {
        std::remove(filepath_.c_str());
        spc_mgr_.reset();   // before the harness pool it borrows
        harness_.reset();
    }

    // Create a doc on the test-ns store via a scoped façade and return its (D-I6) ULID.
    std::string SeedDoc() {
        resource::NamespaceFacade f(harness_->ipool(), "test-ns");
        EXPECT_TRUE(f.Acquire().ok());
        CortrixDoc doc;
        doc.source_type = "test";
        doc.source_path = "/spc/b3_doc_parse.pdf";
        EXPECT_EQ(f.store().doc_create(doc), 0);
        return doc.doc_id;
    }

    // Read back the doc's blocks through a scoped façade (no worker writes concurrently).
    std::vector<CortrixBlock> BlocksOf(const std::string& doc_id) {
        resource::NamespaceFacade f(harness_->ipool(), "test-ns");
        EXPECT_TRUE(f.Acquire().ok());
        std::vector<CortrixBlock> out;
        EXPECT_EQ(f.store().block_get_by_doc(doc_id, out), 0);
        return out;
    }

    // Create a queued task, mark it processing (as the worker would), return it.
    async::TaskInfo SeedProcessingTask(const std::string& doc_id) {
        async::TaskInfo t;
        t.namespace_id = "test-ns";
        t.filename = "big.pdf";   // → InferMimeFromFilename → application/pdf → kBlockFile
        t.filepath = filepath_;
        t.doc_id = doc_id;        // SPC writes under this doc_id (must match the seeded doc)
        t.content_hash = "h-b3";
        auto c = mgr_.CreateTask(t);
        EXPECT_TRUE(c.ok());
        EXPECT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
        return mgr_.GetTask(c.value().task_id).value();
    }

    std::unique_ptr<test::NsPoolHarness> harness_;
    async::TaskManager mgr_;
    InMemoryGlobalConfig cfg_;
    SPCConfig spc_config_;
    spc::ParserFactoryConfig factory_cfg_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<chunker::ParentChildChunker> chunker_;
    std::unique_ptr<BlockAssembler> assembler_;
    spc::NullEnricher enricher_;                 // lifetime ≥ spc_mgr_/pipeline
    std::unique_ptr<spc::DocumentParserFactory> factory_;
    std::unique_ptr<SPCManager> spc_mgr_;        // declared last → destroyed first
    std::string filepath_;
};

// E2E success: a parsed doc handed to the REAL SPCManager::ProcessParsedDoc runs the full
// post-parse pipeline and lands blocks (children + one META) + a P-HNSW point; the task
// finalizes completed. This is what the MockSPCManager path can never prove.
TEST_F(DocParseSpcE2ETest, ParsedDocReachesSpcPipelineAndWritesBlocks) {
    const std::string doc_id = SeedDoc();
    async::DocumentProcessor proc(&mgr_, factory_.get(), &cfg_, nullptr, spc_mgr_.get());

    async::TaskInfo task = SeedProcessingTask(doc_id);
    Status s = proc.ProcessTask(task);
    EXPECT_TRUE(s.ok()) << s.message();

    // Task reached the terminal completed state (handler-owned finalize · §4.1.1).
    EXPECT_EQ(mgr_.GetTask(task.task_id).value().status, async::task_status::kCompleted);

    // Blocks really persisted in the test-ns store: ≥1 child (kBlockFile) + exactly one
    // doc-level META (kBlockMeta), all carrying the doc_id.
    auto blocks = BlocksOf(doc_id);
    ASSERT_GT(blocks.size(), 0u);
    int meta = 0, children = 0;
    for (const auto& b : blocks) {
        EXPECT_EQ(b.doc_id, doc_id);
        if (b.block_type == static_cast<int>(kBlockMeta)) ++meta;
        else if (b.block_type == static_cast<int>(kBlockFile)) ++children;
    }
    EXPECT_EQ(meta, 1) << "exactly one metadata block META block per doc";
    EXPECT_GT(children, 0) << "the chunked children landed as blocks";

    // L3 (default for async large docs) embedded + indexed → the P-HNSW index saw points.
    EXPECT_FALSE(harness_->fake_index()->added_ids().empty());
}

// E2E failure (symmetric): a real SPC write failure (vector AddPoints forced to fail → C2
// rollback) propagates out of ProcessParsedDoc as rc=-1, and DocumentProcessor finalizes
// the task FAILED with CX_ERR_SPC_PROCESS_FAILED — and the rollback leaves no blocks behind.
TEST_F(DocParseSpcE2ETest, SpcWriteFailureRollsBackAndFinalizesFailed) {
    const std::string doc_id = SeedDoc();
    harness_->fake_index()->set_add_should_fail(true);  // L3 AddPoints fails → pipeline rolls back

    async::DocumentProcessor proc(&mgr_, factory_.get(), &cfg_, nullptr, spc_mgr_.get());
    async::TaskInfo task = SeedProcessingTask(doc_id);
    Status s = proc.ProcessTask(task);

    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_SPC_PROCESS_FAILED"), std::string::npos);
    EXPECT_EQ(mgr_.GetTask(task.task_id).value().status, async::task_status::kFailed);

    // C2 rollback wiped any partially-inserted blocks — nothing persisted.
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);
}

// E2E (truly async, main.cpp 7b topology): a kTaskDocParse submitted to the REAL scheduler
// is dispatched by a real WorkerPool thread to the DocumentProcessor handler, which parses
// then runs the real SPC pipeline and lands blocks — closing the full async loop end to end
// (Enqueue → pool dispatch → parse → SPC write → completed), not just a synchronous call.
TEST_F(DocParseSpcE2ETest, DocParseTaskDispatchedThroughPoolWritesBlocks) {
    const std::string doc_id = SeedDoc();
    async::DocumentProcessor proc(&mgr_, factory_.get(), &cfg_, nullptr, spc_mgr_.get());
    SignalingHandler signaling(&proc);

    async::TaskScheduler sched(&mgr_, &cfg_);
    async::WorkerPool pool(&sched, &cfg_);
    pool.RegisterHandler(async::kTaskDocParse, &signaling);
    ASSERT_TRUE(pool.Start().ok());

    // Submit a doc-parse task through the real scheduler (distinct content_hash → no debounce).
    async::SubmitRequest req;
    req.namespace_id = "test-ns";
    req.filename = "big.pdf";
    req.filepath = filepath_;
    req.doc_id = doc_id;
    req.content_hash = "h-b3-async";
    req.total_pages = 1;
    req.task_type = async::kTaskDocParse;
    auto r = sched.Enqueue(req);
    ASSERT_TRUE(r.ok());
    const std::string task_id = r.value().task_id;
    pool.Notify();

    ASSERT_TRUE(signaling.WaitFor(1, std::chrono::seconds(5)));
    pool.Stop();  // join workers before reading the store/FakeIndex (no read/write race)

    EXPECT_TRUE(signaling.last_status().ok()) << signaling.last_status().message();
    EXPECT_EQ(mgr_.GetTask(task_id).value().status, async::task_status::kCompleted);

    // The parse → SPC write really ran on the pool thread: blocks landed + a P-HNSW point.
    EXPECT_GT(BlocksOf(doc_id).size(), 0u);
    EXPECT_FALSE(harness_->fake_index()->added_ids().empty());
}

}  // namespace
}  // namespace cortrix
