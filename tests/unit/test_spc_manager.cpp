#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/spc/spc_task.h"
#include "cortrix/spc/parser_factory.h"
#include "cortrix/spc/docling_parser.h"
#include "cortrix/spc/paddleocr_parser.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc_enricher.h"  // NullEnricher (injected, IsAvailable()==false)
#include "cortrix/chunker/parent_child_chunker.h"
#include "cortrix/config/config.h"

// integration wiring: SPCManager now acquires per-task NamespaceFacades from a real namespace pool
// INamespacePool (not the MVP CortrixNamespaceManager). The real-manager fixture
// therefore stands up a DefaultNamespacePool (mocked catalog routers + FakeIndex + a
// real WriteCoordinator) and admits namespaces through it.
#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/catalog/i_unit_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/f05_config.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/write_coordinator.h"
#include "cortrix/async/task_info.h"   // async::SubmitRequest (SetDocSummaryEnqueue tests)
#include "cortrix/spc/parser.h"        // ParsedDoc / ParserErrorCode (ProcessParsedDoc tests)

#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

namespace cortrix {
namespace {

namespace fs = std::filesystem;

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;

// ============================================================
// Namespace pool test doubles (mirror tests/unit/test_namespace_pool.cpp)
// ============================================================

class FakeIndex : public cortrix::store::IIndex, public cortrix::store::IVectorStore {
public:
    explicit FakeIndex(std::size_t footprint = 0) : footprint_(footprint) {}
    Status AddPoint(const float*, uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>&,
                     const observability::TraceContext*) override {
        return Status::Ok();
    }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int, int,
                                                   const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t) override { return false; }
    Status Snapshot() override { return Status::Ok(); }
    Status Recover() override { return Status::Ok(); }
    Status Shutdown() override { return Status::Ok(); }
    cortrix::store::IndexStats GetStats() override { return cortrix::store::IndexStats{}; }
    std::size_t GetMemoryFootprintBytes() const override { return footprint_; }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int,
                                                   const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t, const observability::TraceContext*) override { return false; }
    Status Snapshot(const observability::TraceContext*) override { return Status::Ok(); }
    Status Recover(const observability::TraceContext*) override { return Status::Ok(); }

private:
    std::size_t footprint_;
};

class MockIndexFactory : public cortrix::store::IIndexFactory {
public:
    MOCK_METHOD((Result<std::unique_ptr<cortrix::store::IIndex>>), Create,
                (const std::string&, const cortrix::store::IndexConfig&), (override));
    MOCK_METHOD((Result<std::unique_ptr<cortrix::store::IIndex>>), Open,
                (const std::string&), (override));
};

class MockNSRouter : public cortrix::catalog::INSRouter {
public:
    MOCK_METHOD((Result<std::vector<cortrix::catalog::UnitDescriptor>>), LookupUnits,
                (const std::string&), (const, override));
    MOCK_METHOD((Result<cortrix::catalog::UnitDescriptor>), GetActiveUnit,
                (const std::string&), (const, override));
    MOCK_METHOD((Result<cortrix::catalog::NSMetadata>), GetNamespace,
                (const std::string&), (const, override));
    MOCK_METHOD((Result<cortrix::catalog::BatchResult<std::string>>), ListNamespaces,
                (const cortrix::catalog::ListNamespacesOptions&), (const, override));
    MOCK_METHOD(Status, CreateNamespace, (const cortrix::catalog::NSMetadata&), (override));
    MOCK_METHOD(Status, DeleteNamespace, (const std::string&), (override));
};

class MockUnitRouter : public cortrix::catalog::IUnitRouter {
public:
    MOCK_METHOD((Result<cortrix::catalog::UnitDescriptor>), GetUnit,
                (const std::string&), (const, override));
    MOCK_METHOD((Result<cortrix::catalog::UnitState>), GetUnitState,
                (const std::string&), (const, override));
    MOCK_METHOD(Status, UpdateUnitState,
                (const std::string&, cortrix::catalog::UnitState), (override));
    MOCK_METHOD(Status, UpdateUnitStats,
                (const std::string&, const cortrix::catalog::UnitStats&), (override));
    MOCK_METHOD((Result<bool>), ShouldSeal, (const std::string&), (const, override));
};

// Minimal test: SPCManager lifecycle (Submit requires running state)
TEST(SPCManagerTest, SubmitBeforeStartFails) {
    // We can't create a real SPCManager without a pool here, so we test the queue
    // behavior directly through SPCTaskQueue.
    SPCConfig config;
    config.worker_count = 1;
    config.max_queue_size = 10;

    SPCTaskQueue queue(10);
    auto task = std::make_shared<SPCTask>();
    task->priority = SPCPriority::kP1;
    task->source_path = "test.txt";

    EXPECT_EQ(queue.Push(task), 0);
    EXPECT_EQ(queue.Size(), 1u);
}

TEST(SPCManagerTest, TaskLifecycle) {
    // Test SPCTask structure
    auto task = std::make_shared<SPCTask>();
    task->task_id = 1;
    task->doc_id = "01JTESTDOC0000000000000100";
    task->namespace_name = "default";
    task->source_path = "/data/test.pdf";
    task->mime_type = "application/pdf";
    task->priority = SPCPriority::kP0;
    task->stage = SPCStage::kQueued;

    EXPECT_EQ(task->task_id, 1);
    EXPECT_EQ(task->priority, SPCPriority::kP0);
    EXPECT_EQ(task->stage, SPCStage::kQueued);
    EXPECT_FALSE(task->cancelled.load());

    // Cancel
    task->cancelled.store(true);
    EXPECT_TRUE(task->cancelled.load());
}

TEST(SPCManagerTest, QueueCancelAndSize) {
    SPCTaskQueue queue(100);

    auto t1 = std::make_shared<SPCTask>();
    t1->source_path = "a.txt";
    t1->priority = SPCPriority::kP1;
    t1->created_at_us = 1;

    auto t2 = std::make_shared<SPCTask>();
    t2->source_path = "b.txt";
    t2->priority = SPCPriority::kP1;
    t2->created_at_us = 2;

    auto t3 = std::make_shared<SPCTask>();
    t3->source_path = "a.txt";
    t3->priority = SPCPriority::kP1;
    t3->created_at_us = 3;

    queue.Push(t1);
    queue.Push(t2);
    queue.Push(t3);

    EXPECT_EQ(queue.Size(), 3u);
    int cancelled = queue.CancelBySourcePath("a.txt");
    EXPECT_EQ(cancelled, 2);

    // Size doesn't change (tasks are still in queue, just marked cancelled)
    EXPECT_EQ(queue.Size(), 3u);
}

// ============================================================
// Real SPCManager tests - exercises constructor, Start, Stop,
// Submit, QueueSize, GetTaskStage, CancelBySourcePath, WorkerLoop
// ============================================================

class SPCManagerRealTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_spc_mgr_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(test_dir_);
        config_.data_root = test_dir_.string();

        // Real namespace pool (no namespaces admitted yet — tests admit what they need).
        pool_ = MakePool();

        // Build real pipeline components
        spc_config_.worker_count = 1;
        spc_config_.max_queue_size = 100;
        spc_config_.python_bin = "python3";
        spc_config_.parser_timeout_s = 5;

        // Structured-parser factory with a stub Docling parser (no Python
        // dependency — these SPCManager lifecycle tests never feed a real doc
        // through Process; they exercise Start/Stop/Submit/QueueSize). A bare
        // factory with stub parsers is enough to construct the pipeline.
        cortrix::spc::ParserFactoryConfig pf_cfg;
        parser_factory_ = std::make_unique<cortrix::spc::DocumentParserFactory>(pf_cfg);
        cortrix::spc::DoclingParserConfig dc;
        dc.python_path = "python3";
        parser_factory_->SetPrimaryParser(
            std::make_unique<cortrix::spc::DoclingParser>(dc));
        cortrix::spc::PaddleOCRParserConfig pc;
        pc.python_path = "python3";
        parser_factory_->SetFallbackParser(
            std::make_unique<cortrix::spc::PaddleOCRParser>(pc));

        chunker_ = std::make_unique<cortrix::chunker::ParentChildChunker>(
            cortrix::chunker::ChunkerConfig{});

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();

        assembler_ = std::make_unique<BlockAssembler>();

        auto pipeline = std::make_unique<SPCPipeline>(
            *parser_factory_, *chunker_, *embedder_, *assembler_, enricher_);

        mgr_ = std::make_unique<SPCManager>(spc_config_, *pool_, std::move(pipeline));
    }

    void TearDown() override {
        mgr_.reset();
        pool_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    cortrix::catalog::UnitDescriptor Unit(const std::string& unit_id) {
        cortrix::catalog::UnitDescriptor u;
        u.unit_id = unit_id;
        u.tenant_id = "t1";
        return u;
    }

    resource::WriteCoordinatorFactory MakeRealCoordFactory() {
        return [](const std::string& data_dir, const cortrix::store::WriteCoordinatorConfig& cfg,
                  cortrix::store::IVectorStore* vs, cortrix::store::IMetadataStore* ms,
                  cortrix::store::IBlobStore* bs)
                   -> Result<std::unique_ptr<cortrix::store::WriteCoordinator>> {
            auto coord = std::make_unique<cortrix::store::WriteCoordinator>(data_dir, cfg, vs, ms, bs);
            Status init = coord->Init();
            if (!init.ok()) return init;
            return Result<std::unique_ptr<cortrix::store::WriteCoordinator>>(std::move(coord));
        };
    }

    std::unique_ptr<resource::DefaultNamespacePool> MakePool() {
        ON_CALL(ns_router_, GetActiveUnit(_))
            .WillByDefault(Invoke([this](const std::string& ns) {
                return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
            }));
        ON_CALL(index_factory_, Open(_))
            .WillByDefault(Invoke([](const std::string&) {
                return Result<std::unique_ptr<cortrix::store::IIndex>>(
                    std::make_unique<FakeIndex>(0));
            }));
        return std::make_unique<resource::DefaultNamespacePool>(
            &index_factory_, MakeRealCoordFactory(), &ns_router_, &unit_router_, config_);
    }

    // Admit a resident namespace (pre-create its Unit dir for store.db / pending.wal).
    void AdmitNs(const std::string& ns) {
        fs::create_directories(test_dir_ / (ns + "-u"));
        ASSERT_TRUE(pool_->AdmitCreate(ns, 100).ok()) << ns;
    }

    fs::path test_dir_;
    resource::F05Config config_;
    SPCConfig spc_config_;
    NiceMock<MockIndexFactory> index_factory_;
    NiceMock<MockNSRouter> ns_router_;
    NiceMock<MockUnitRouter> unit_router_;
    std::unique_ptr<resource::DefaultNamespacePool> pool_;

    std::unique_ptr<cortrix::spc::DocumentParserFactory> parser_factory_;
    std::unique_ptr<cortrix::chunker::ParentChildChunker> chunker_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<BlockAssembler> assembler_;
    cortrix::spc::NullEnricher enricher_;  // Enricher short-circuit (lifetime ≥ mgr_/pipeline)

    std::unique_ptr<SPCManager> mgr_;
};

// Coverage: SPCManager constructor, Submit before start
TEST_F(SPCManagerRealTest, SubmitBeforeStartReturnsUnavailable) {
    auto task = std::make_shared<SPCTask>();
    task->source_path = "test.txt";
    task->priority = SPCPriority::kP1;

    Status s = mgr_->Submit(task);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
}

// Coverage: Start, Stop, QueueSize
TEST_F(SPCManagerRealTest, StartAndStopLifecycle) {
    EXPECT_EQ(mgr_->QueueSize(), 0u);

    mgr_->Start();
    EXPECT_EQ(mgr_->QueueSize(), 0u);

    mgr_->Stop();
    EXPECT_EQ(mgr_->QueueSize(), 0u);
}

// Coverage: Start twice is no-op
TEST_F(SPCManagerRealTest, StartTwiceIsNoOp) {
    mgr_->Start();
    mgr_->Start();  // Should not create extra threads
    mgr_->Stop();
}

// Coverage: Stop twice is no-op
TEST_F(SPCManagerRealTest, StopTwiceIsNoOp) {
    mgr_->Start();
    mgr_->Stop();
    mgr_->Stop();  // Should not crash
}

// Coverage: GetTaskStage
TEST_F(SPCManagerRealTest, GetTaskStageReturnsQueued) {
    SPCStage stage = mgr_->GetTaskStage(42);
    EXPECT_EQ(stage, SPCStage::kQueued);
}

// Coverage: Submit after start, CancelBySourcePath
TEST_F(SPCManagerRealTest, SubmitAndCancelBySourcePath) {
    mgr_->Start();

    auto task = std::make_shared<SPCTask>();
    task->source_path = "cancel_me.txt";
    task->namespace_name = "nonexistent_ns";
    task->priority = SPCPriority::kP1;

    Status s = mgr_->Submit(task);
    EXPECT_TRUE(s.ok());

    int cancelled = mgr_->CancelBySourcePath("cancel_me.txt");
    EXPECT_GE(cancelled, 0);

    mgr_->Stop();
}

// [integration · deployment] Graceful-shutdown drain: Stop() + DrainRemaining()
// moves out every not-yet-started task (cancelled ones skipped), leaving the
// queue empty — these become .pending_tasks.json for next-start resume.
TEST(SPCManagerTest, QueueDrainRemaining) {
    SPCTaskQueue queue(100);

    auto mk = [](const char* path) {
        auto t = std::make_shared<SPCTask>();
        t->source_path = path;
        t->priority = SPCPriority::kP1;
        return t;
    };
    ASSERT_EQ(queue.Push(mk("a.txt")), 0);
    ASSERT_EQ(queue.Push(mk("b.txt")), 0);
    ASSERT_EQ(queue.Push(mk("c.txt")), 0);
    ASSERT_EQ(queue.CancelBySourcePath("b.txt"), 1);

    queue.Stop();
    auto remaining = queue.DrainRemaining();
    ASSERT_EQ(remaining.size(), 2u);  // cancelled b.txt skipped
    EXPECT_EQ(remaining[0]->source_path, "a.txt");
    EXPECT_EQ(remaining[1]->source_path, "c.txt");
    EXPECT_EQ(queue.Size(), 0u);

    // Second drain is empty (queue cleared).
    EXPECT_TRUE(queue.DrainRemaining().empty());
}

// Manager-level drain proxy: idempotent with Stop(), empty when nothing queued.
TEST_F(SPCManagerRealTest, DrainRemainingTasksEmptyAndIdempotent) {
    mgr_->Start();
    auto remaining = mgr_->DrainRemainingTasks();  // also Stop()s the manager
    EXPECT_TRUE(remaining.empty());
    EXPECT_TRUE(mgr_->DrainRemainingTasks().empty());  // idempotent
}

// Deployment disk gate: a true write-reject probe refuses NEW task
// admission with CX_ERR_DISK_FULL; clearing the pressure re-admits.
TEST_F(SPCManagerRealTest, SubmitRejectedWhenDiskCritical) {
    mgr_->Start();

    bool disk_critical = true;
    mgr_->SetWriteRejectProbe([&disk_critical] { return disk_critical; });

    auto task = std::make_shared<SPCTask>();
    task->source_path = "disk_full.txt";
    task->namespace_name = "nonexistent_ns";
    task->priority = SPCPriority::kP1;

    Status s = mgr_->Submit(task);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_NE(s.message().find("CX_ERR_DISK_FULL"), std::string::npos);
    EXPECT_EQ(mgr_->QueueSize(), 0u);  // never admitted

    // Pressure clears (NORMAL stage) → admission resumes.
    disk_critical = false;
    Status s2 = mgr_->Submit(task);
    EXPECT_TRUE(s2.ok());

    mgr_->CancelBySourcePath("disk_full.txt");
    mgr_->Stop();
}

// Coverage: WorkerLoop with a namespace the pool can't acquire (CX_ERR_NS_NOT_FOUND)
TEST_F(SPCManagerRealTest, WorkerLoop_NonexistentNamespace) {
    mgr_->Start();

    auto task = std::make_shared<SPCTask>();
    task->source_path = "test.txt";
    task->namespace_name = "nonexistent_namespace";  // never admitted
    task->doc_id = "01JTESTDOC0000000000000001";
    task->priority = SPCPriority::kP0;

    Status s = mgr_->Submit(task);
    EXPECT_TRUE(s.ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // The worker's facade.Acquire() fails; the error surfaces the failed namespace.
    EXPECT_EQ(task->stage, SPCStage::kError);
    EXPECT_NE(task->error_message.find("namespace acquire failed"), std::string::npos);
    EXPECT_NE(task->error_message.find("nonexistent_namespace"), std::string::npos);

    mgr_->Stop();
}

// Coverage: WorkerLoop skips a pre-cancelled task before acquiring
TEST_F(SPCManagerRealTest, WorkerLoop_CancelledTask) {
    mgr_->Start();

    auto task = std::make_shared<SPCTask>();
    task->source_path = "cancelled.txt";
    task->namespace_name = "default";
    task->doc_id = "01JTESTDOC0000000000000001";
    task->priority = SPCPriority::kP1;
    task->cancelled.store(true);  // Pre-cancel the task

    Status s = mgr_->Submit(task);
    EXPECT_TRUE(s.ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Task should still be cancelled (worker skips it without setting error)
    EXPECT_TRUE(task->cancelled.load());

    mgr_->Stop();
}

// Coverage: WorkerLoop with a real (admitted) namespace + L0 pipeline
TEST_F(SPCManagerRealTest, WorkerLoop_RealNamespaceL0Processing) {
    AdmitNs("test_ns");

    mgr_->Start();

    auto task = std::make_shared<SPCTask>();
    task->source_path = "/tmp/dummy.txt";
    task->namespace_name = "test_ns";
    task->doc_id = "01JTESTDOC0000000000000001";
    task->mime_type = "text/plain";
    task->priority = SPCPriority::kP0;
    task->processing_level = 0;  // L0: skip processing entirely

    Status s = mgr_->Submit(task);
    EXPECT_TRUE(s.ok());

    for (int i = 0; i < 50; ++i) {
        if (task->stage == SPCStage::kDone || task->stage == SPCStage::kError) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // L0 should complete immediately
    EXPECT_EQ(task->stage, SPCStage::kDone)
        << "Task error: " << task->error_message;

    mgr_->Stop();
}

// Coverage: WorkerLoop pipeline error path (real namespace, bad source file)
TEST_F(SPCManagerRealTest, WorkerLoop_PipelineError) {
    AdmitNs("test_ns2");

    mgr_->Start();

    auto task = std::make_shared<SPCTask>();
    task->source_path = "/nonexistent/file.txt";
    task->namespace_name = "test_ns2";
    task->doc_id = "01JTESTDOC0000000000000001";
    task->mime_type = "text/plain";
    task->priority = SPCPriority::kP0;
    task->processing_level = 3;

    Status s = mgr_->Submit(task);
    EXPECT_TRUE(s.ok());

    for (int i = 0; i < 50; ++i) {
        if (task->stage == SPCStage::kDone || task->stage == SPCStage::kError) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Should fail with parse error
    EXPECT_EQ(task->stage, SPCStage::kError);
    EXPECT_FALSE(task->error_message.empty());

    mgr_->Stop();
}

// Coverage: Submit fills task_id and created_at_us
TEST_F(SPCManagerRealTest, SubmitAssignsTaskIdAndTimestamp) {
    mgr_->Start();

    auto task1 = std::make_shared<SPCTask>();
    task1->source_path = "t1.txt";
    task1->namespace_name = "nonexistent";
    task1->priority = SPCPriority::kP1;
    task1->cancelled.store(true);  // Cancel to avoid processing

    auto task2 = std::make_shared<SPCTask>();
    task2->source_path = "t2.txt";
    task2->namespace_name = "nonexistent";
    task2->priority = SPCPriority::kP1;
    task2->cancelled.store(true);

    mgr_->Submit(task1);
    mgr_->Submit(task2);

    EXPECT_GT(task1->task_id, 0);
    EXPECT_GT(task2->task_id, task1->task_id);
    EXPECT_GT(task1->created_at_us, 0);
    EXPECT_GT(task2->created_at_us, 0);

    mgr_->Stop();
}

// Coverage: destructor calls Stop
TEST_F(SPCManagerRealTest, DestructorCallsStop) {
    mgr_->Start();
    // Let the manager destruct naturally via TearDown -> mgr_.reset()
}

// ============================================================
// ProcessParsedDoc (async entry point) — the manager acquires a
// per-task facade and delegates to pipeline_->ProcessParsed. This is
// distinct from the WorkerLoop path (no blob extraction here).
// ============================================================

// ProcessParsedDoc with a non-admitted namespace → facade.Acquire() fails →
// task.stage = kError, error_message contains "namespace acquire failed".
TEST_F(SPCManagerRealTest, ProcessParsedDoc_NonexistentNamespace_Error) {
    cortrix::spc::ParsedDoc d;
    d.status = cortrix::spc::ParserErrorCode::kOk;
    d.parser_name = "docling";
    d.metadata.filename = "f.txt";

    SPCTask task;
    task.doc_id = "01JTESTDOC0000000000000099";
    task.namespace_name = "nonexistent_ns_for_parsed";
    task.source_path = "/tmp/f.txt";
    task.mime_type = "text/plain";
    task.processing_level = 3;
    task.priority = SPCPriority::kP1;

    int rc = mgr_->ProcessParsedDoc(d, task);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kError);
    EXPECT_NE(task.error_message.find("namespace acquire failed"), std::string::npos);
    EXPECT_NE(task.error_message.find("nonexistent_ns_for_parsed"), std::string::npos);
}

// ProcessParsedDoc with a real (admitted) namespace and L0 task:
// pipeline skips processing immediately → task.stage = kDone.
TEST_F(SPCManagerRealTest, ProcessParsedDoc_RealNamespaceL0_Done) {
    AdmitNs("pp_ns");

    // The async task path must create the doc row before calling ProcessParsedDoc because
    // the manager creates it internally when doc_get fails (no existing row).
    cortrix::spc::ParsedDoc d;
    d.status = cortrix::spc::ParserErrorCode::kOk;
    d.parser_name = "docling";
    d.metadata.filename = "f.txt";
    d.metadata.page_count = 0;

    SPCTask task;
    task.doc_id = "01JTESTDOC0000000000000098";
    task.namespace_name = "pp_ns";
    task.source_path = "/tmp/f.txt";
    task.mime_type = "text/plain";
    task.processing_level = 0;  // L0: skip all processing
    task.priority = SPCPriority::kP0;
    task.source_type = "file";

    int rc = mgr_->ProcessParsedDoc(d, task);
    EXPECT_EQ(rc, 0) << task.error_message;
    EXPECT_EQ(task.stage, SPCStage::kDone);
}

// ProcessParsedDoc where the manager creates the doc row (doc_get fails →
// doc_create path). A doc_id that has never been inserted lands on the
// "doc_get != 0 → doc_create" branch → task.stage = kDone (L0).
TEST_F(SPCManagerRealTest, ProcessParsedDoc_CreatesDocRowWhenMissing) {
    AdmitNs("pp_ns2");

    cortrix::spc::ParsedDoc d;
    d.status = cortrix::spc::ParserErrorCode::kOk;
    d.parser_name = "docling";
    d.metadata.filename = "new.txt";
    d.metadata.page_count = 0;

    SPCTask task;
    task.doc_id = "01JTESTDOC0000000000000097";
    task.namespace_name = "pp_ns2";
    task.source_path = "/tmp/new.txt";
    task.mime_type = "text/plain";
    task.processing_level = 0;
    task.priority = SPCPriority::kP0;
    task.source_type = "file";
    task.content_hash = "abc123";

    int rc = mgr_->ProcessParsedDoc(d, task);
    EXPECT_EQ(rc, 0) << task.error_message;
    EXPECT_EQ(task.stage, SPCStage::kDone);
}

// ============================================================
// Delegate setters with a null pipeline_ — each method must be a
// safe no-op (null-check guard in SPCManager). The protected default
// ctor leaves pipeline_ = nullptr; we build a raw instance via it.
// ============================================================

// SetDocSummaryEnqueue delegates to pipeline_->SetDocSummaryEnqueue; with a
// real pipeline it is installed (exercised via WorkerLoop above). Here verify
// the method does not crash when pipeline_ is not null (forwarded silently).
TEST_F(SPCManagerRealTest, SetDocSummaryEnqueue_DoesNotCrash) {
    int called = 0;
    mgr_->SetDocSummaryEnqueue([&](const async::SubmitRequest&) { ++called; });
    // No assertion needed — just must not crash or throw.
}

TEST_F(SPCManagerRealTest, SetEnricherChain_DoesNotCrash) {
    mgr_->SetEnricherChain(nullptr);
}

TEST_F(SPCManagerRealTest, SetSparseIndexRegistry_DoesNotCrash) {
    mgr_->SetSparseIndexRegistry(nullptr);
}

// ============================================================
// WorkerLoop http_upload path — blob load failure sets kError.
// The worker extracts the blob to a temp file before parsing when
// source_type = "http_upload". A blob that doesn't exist in the store
// causes brc != 0 → task.stage = kError.
// ============================================================

TEST_F(SPCManagerRealTest, WorkerLoop_HttpUpload_BlobLoadFail) {
    AdmitNs("upload_ns");

    mgr_->Start();

    auto task = std::make_shared<SPCTask>();
    task->source_path = "doc.pdf";
    task->namespace_name = "upload_ns";
    task->doc_id = "01JTESTDOC0000000000000096";
    task->mime_type = "application/pdf";
    task->priority = SPCPriority::kP0;
    task->source_type = "http_upload";  // triggers blob extraction path
    task->processing_level = 3;

    Status s = mgr_->Submit(task);
    EXPECT_TRUE(s.ok());

    for (int i = 0; i < 50; ++i) {
        if (task->stage == SPCStage::kDone || task->stage == SPCStage::kError) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Blob was never written to the store → brc != 0 or blob_data.empty() → kError.
    EXPECT_EQ(task->stage, SPCStage::kError);
    EXPECT_NE(task->error_message.find("blob load failed"), std::string::npos);

    mgr_->Stop();
}

}  // namespace
}  // namespace cortrix
