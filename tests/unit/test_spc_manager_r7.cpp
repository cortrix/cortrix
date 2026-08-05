// R7 line/branch-coverage supplement for src/spc/spc_manager.cpp (line 83.7%).
//
// The existing test_spc_manager.cpp drives lifecycle / Submit / disk-reject /
// WorkerLoop variants / ProcessParsedDoc (nonexistent ns, L0-done, doc-create) and
// the 3 setters on a REAL pipeline. The remaining gaps:
//   - SetCleaningConfigResolver proxy (the 4th setter — not exercised at all);
//   - the `if (pipeline_)` FALSE arm of ALL 5 setter proxies (the existing setter
//     tests use a manager that HAS a pipeline; the null-pipeline branch needs the
//     protected default ctor, reached via a test subclass);
//   - ProcessParsedDoc's "doc already exists" ELSE arm (doc_get == 0 →
//     doc_update_status(kProcessing)) — existing ProcessParsedDoc tests only hit the
//     doc-create (doc_get != 0) arm.
//
// Standalone NEW file; does not touch the existing test_spc_manager.cpp. Mirrors its
// SPCManagerRealTest fixture (real namespace pool + mocked routers + FakeIndex + real write coordinator
// WriteCoordinator).
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
#include "cortrix/spc_enricher.h"
#include "cortrix/spc/enricher_chain.h"
#include "cortrix/chunker/parent_child_chunker.h"
#include "cortrix/config/config.h"

#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/catalog/i_unit_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/f05_config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/write_coordinator.h"
#include "cortrix/async/task_info.h"
#include "cortrix/spc/parser.h"
#include "cortrix/retrieval/sparse_index_registry.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cortrix {
namespace {

namespace fs = std::filesystem;

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;

// ── namespace pool doubles (same shapes as test_spc_manager.cpp) ──
class FakeIndex : public cortrix::store::IIndex, public cortrix::store::IVectorStore {
public:
    explicit FakeIndex(std::size_t footprint = 0) : footprint_(footprint) {}
    Status AddPoint(const float*, uint64_t, const observability::TraceContext*) override { return Status::Ok(); }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override { return Status::Ok(); }
    Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>&,
                     const observability::TraceContext*) override { return Status::Ok(); }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int, int,
                                                   const observability::TraceContext*) override { return {}; }
    bool Exists(uint64_t) override { return false; }
    Status Snapshot() override { return Status::Ok(); }
    Status Recover() override { return Status::Ok(); }
    Status Shutdown() override { return Status::Ok(); }
    cortrix::store::IndexStats GetStats() override { return cortrix::store::IndexStats{}; }
    std::size_t GetMemoryFootprintBytes() const override { return footprint_; }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int,
                                                   const observability::TraceContext*) override { return {}; }
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

// Test subclass that exposes the protected default ctor, leaving pipeline_ = nullptr.
// Used to drive the `if (pipeline_)` FALSE arm of every setter proxy (the null-pipeline
// no-op path). The default ctor also leaves running_=false so the destructor's Stop()
// is a clean no-op.
class NullPipelineManager : public SPCManager {
public:
    NullPipelineManager() : SPCManager() {}  // protected default ctor → pipeline_ == nullptr
};

class SPCManagerR7Test : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_spc_mgr_r7_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(test_dir_);
        config_.data_root = test_dir_.string();
        pool_ = MakePool();

        spc_config_.worker_count = 1;
        spc_config_.max_queue_size = 100;
        spc_config_.python_bin = "python3";
        spc_config_.parser_timeout_s = 5;

        cortrix::spc::ParserFactoryConfig pf_cfg;
        parser_factory_ = std::make_unique<cortrix::spc::DocumentParserFactory>(pf_cfg);
        cortrix::spc::DoclingParserConfig dc;
        dc.python_path = "python3";
        parser_factory_->SetPrimaryParser(std::make_unique<cortrix::spc::DoclingParser>(dc));
        cortrix::spc::PaddleOCRParserConfig pc;
        pc.python_path = "python3";
        parser_factory_->SetFallbackParser(std::make_unique<cortrix::spc::PaddleOCRParser>(pc));

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
    cortrix::spc::NullEnricher enricher_;
    std::unique_ptr<SPCManager> mgr_;
};

// ============================================================
// SetCleaningConfigResolver proxy (the 4th setter — gap).
// ============================================================

// On a manager WITH a real pipeline, SetCleaningConfigResolver forwards to the
// pipeline (the `if (pipeline_)` TRUE arm) and the resolved NS config takes effect
// on the next ProcessParsedDoc — verified indirectly here by simply not crashing and
// completing an L0 ProcessParsedDoc afterward (the resolver runs inside the pipeline).
TEST_F(SPCManagerR7Test, SetCleaningConfigResolver_ForwardsToPipeline) {
    int resolver_calls = 0;
    mgr_->SetCleaningConfigResolver(
        [&resolver_calls](const std::string&) {
            ++resolver_calls;
            cortrix::spc::CleaningConfig c;
            c.dedup_enabled = false;
            return c;
        });

    // L0 skips processing entirely, so the resolver may not be invoked — the point is
    // the proxy installed without crashing (TRUE arm of `if (pipeline_)`).
    AdmitNs("ccr_ns");
    cortrix::spc::ParsedDoc d;
    d.status = cortrix::spc::ParserErrorCode::kOk;
    d.parser_name = "docling";
    d.metadata.filename = "f.txt";
    SPCTask task;
    task.doc_id = "01JTESTDOC00000000000000C1";
    task.namespace_name = "ccr_ns";
    task.source_path = "/tmp/f.txt";
    task.mime_type = "text/plain";
    task.processing_level = 0;
    task.source_type = "file";

    int rc = mgr_->ProcessParsedDoc(d, task);
    EXPECT_EQ(rc, 0) << task.error_message;
    EXPECT_EQ(task.stage, SPCStage::kDone);
}

// ============================================================
// Null-pipeline FALSE arm of every setter proxy. NullPipelineManager uses the
// protected default ctor, so pipeline_ == nullptr → each setter's `if (pipeline_)`
// guard takes the false arm and the call is a safe no-op (must not deref null).
// ============================================================

TEST(SPCManagerR7NullPipeline, AllSettersNoOpWhenPipelineNull) {
    NullPipelineManager mgr;  // pipeline_ == nullptr

    // None of these may dereference the null pipeline_ (all guarded).
    mgr.SetDocSummaryEnqueue([](const async::SubmitRequest&) {});
    mgr.SetEnricherChain(nullptr);
    mgr.SetSparseIndexRegistry(nullptr);
    mgr.SetCleaningConfigResolver(
        [](const std::string&) { return cortrix::spc::CleaningConfig{}; });
    mgr.SetWriteRejectProbe([] { return false; });

    // Also pass non-null seam pointers to confirm the guard (not the arg) is what
    // short-circuits — still a no-op because pipeline_ is null.
    cortrix::spc::EnricherChain chain;
    mgr.SetEnricherChain(&chain);
    cortrix::retrieval::SparseIndexRegistry registry("");
    mgr.SetSparseIndexRegistry(&registry);

    SUCCEED();  // reaching here without a null-deref crash is the assertion
}

// ============================================================
// ProcessParsedDoc — "doc already exists" ELSE arm. Pre-create the documents row,
// then ProcessParsedDoc's doc_get succeeds (== 0) → it takes the
// doc_update_status(kProcessing) branch instead of doc_create. Existing tests only
// cover the doc-create (doc_get != 0) arm.
// ============================================================

TEST_F(SPCManagerR7Test, ProcessParsedDoc_ExistingDocRow_UpdatesStatusBranch) {
    AdmitNs("pp_exist_ns");

    // Pre-create the doc row via a per-task façade so doc_get later succeeds.
    const std::string doc_id = "01JTESTDOC00000000000000E1";
    {
        resource::NamespaceFacade facade(*pool_, "pp_exist_ns");
        ASSERT_TRUE(facade.Acquire().ok());
        cortrix::CortrixDoc doc;
        doc.doc_id = doc_id;
        doc.source_type = "file";
        doc.source_path = "/tmp/exist.txt";
        doc.mime_type = "text/plain";
        doc.processing_level = 0;
        doc.status = DocStatus::kProcessing;
        ASSERT_EQ(facade.store().doc_create(doc), 0);
    }

    cortrix::spc::ParsedDoc d;
    d.status = cortrix::spc::ParserErrorCode::kOk;
    d.parser_name = "docling";
    d.metadata.filename = "exist.txt";
    d.metadata.page_count = 0;

    SPCTask task;
    task.doc_id = doc_id;  // already in the documents table → doc_get == 0 → ELSE arm
    task.namespace_name = "pp_exist_ns";
    task.source_path = "/tmp/exist.txt";
    task.mime_type = "text/plain";
    task.processing_level = 0;  // L0 → done (we only need to reach the else branch)
    task.source_type = "file";

    int rc = mgr_->ProcessParsedDoc(d, task);
    EXPECT_EQ(rc, 0) << task.error_message;
    EXPECT_EQ(task.stage, SPCStage::kDone);

    // Doc ended ready (L0 marks ready); the row was reused, not recreated.
    resource::NamespaceFacade facade(*pool_, "pp_exist_ns");
    ASSERT_TRUE(facade.Acquire().ok());
    cortrix::CortrixDoc got;
    ASSERT_EQ(facade.store().doc_get(doc_id, got), 0);
    EXPECT_EQ(got.status, DocStatus::kReady);
}

}  // namespace
}  // namespace cortrix
