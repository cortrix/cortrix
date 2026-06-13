#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/spc/parser_factory.h"
#include "cortrix/spc/docling_parser.h"
#include "cortrix/spc/paddleocr_parser.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc_enricher.h"  // F03 NullEnricher (injected, IsAvailable()==false)
#include "cortrix/spc/spc_router.h"
#include "cortrix/spc/spc_task.h"
#include "cortrix/chunker/parent_child_chunker.h"

// [A unified-blocks Inc 4-3] The pipeline now runs the F06 structured parser
// (DocumentParserFactory::ParseDocument → ParsedDoc) → F34 ParentChildChunker →
// unified write (parents into `parents`, children into `blocks`). The fixture
// injects a *mock* Docling bridge (a python script that prints a fixed §3.1
// page-level JSON envelope — identical plumbing to test_parser_fallback.cpp) so
// the orchestration is exercised standalone (the machine has python3 but not
// docling). Writes are verified against real SQLite (block_get_by_doc / parent_get
// / doc status) and the captured FakeIndex (added_ids), exactly as production
// wires it via the F05 NamespaceFacade.
#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/catalog/i_unit_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/f05_config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/resource/namespace_resource_bundle.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/store/cortrix_store_sqlite.h"  // SetFailNextOps seam (F23 section 4.5)
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/write_coordinator.h"
#include "cortrix/id/hash.h"
#include "cortrix/id/ulid.h"

#include "cortrix/async/task_info.h"
#include "cortrix/common/block_types.h"
#include "cortrix/common/block_header.h"
#include "cortrix/config/config.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include "mock_llm_client.h"  // F03 ③b: drive an available LlmEnricher without a live endpoint

namespace cortrix {
namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;

// ============================================================
// Test doubles (mirror tests/unit/test_namespace_pool.cpp — the F05 pool template)
// ============================================================

// IIndex + IVectorStore stand-in. CAPTURES the ids handed to AddPoint/AddPoints
// (so a test can assert the L3 vector write went through facade.vec_index()) and
// can be told to fail (the C2 rollback path). PHnsw implements both bases, so the
// pool's cross-cast to IVectorStore (D3.5 C1) succeeds against this fake too.
class FakeIndex : public cortrix::store::IIndex, public cortrix::store::IVectorStore {
public:
    explicit FakeIndex(std::size_t footprint = 0) : footprint_(footprint) {}

    // —— shared by IIndex + IVectorStore (identical signatures) ——
    Status AddPoint(const float*, uint64_t id, const observability::TraceContext*) override {
        if (add_should_fail_) return Status(StatusCode::kUnavailable, "forced add failure");
        added_ids_.push_back(id);
        return Status::Ok();
    }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    // —— IIndex-only ——
    Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>& pts,
                     const observability::TraceContext*) override {
        if (add_should_fail_) return Status(StatusCode::kUnavailable, "forced add failure");
        for (const auto& p : pts) added_ids_.push_back(p.second);
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
    // —— IVectorStore-only ——
    std::vector<std::pair<uint64_t, float>> Search(const float*, int,
                                                   const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t, const observability::TraceContext*) override { return false; }
    Status Snapshot(const observability::TraceContext*) override { return Status::Ok(); }
    Status Recover(const observability::TraceContext*) override { return Status::Ok(); }

    void set_add_should_fail(bool v) { add_should_fail_ = v; }
    const std::vector<uint64_t>& added_ids() const { return added_ids_; }

private:
    std::size_t footprint_;
    std::vector<uint64_t> added_ids_;
    bool add_should_fail_ = false;
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

// Multi-paragraph Docling payload (high confidence → no fallback). Enough text to
// produce one parent + multiple children under EstimateTokens / the default
// ChunkerConfig (parent_size=1024, child_size=200).
const char* kDoclingMultiPara = R"JSON({
  "status": 0, "parser": "docling",
  "metadata": {"filename": "doc.txt", "page_count": 1, "doc_language": "en"},
  "pages": [{"page_num": 1, "page_text": "para one para two para three",
             "page_metadata": {"page_num": 1, "parser_used": "docling",
                               "page_confidence": 0.95, "is_scan_page": false,
                               "char_count": 28},
             "paragraphs": [
               {"text": "PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE PARAGRAPH_BODY_ONE",
                "page": 1, "type": "TEXT", "confidence": 0.95, "language": "en"},
               {"text": "PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO PARAGRAPH_BODY_TWO",
                "page": 1, "type": "TEXT", "confidence": 0.95, "language": "en"}
             ]}]})JSON";

// Empty Docling payload (zero pages → CX_ERR_CHUNK_EMPTY_DOCUMENT downstream).
const char* kDoclingEmpty = R"JSON({
  "status": 0, "parser": "docling",
  "metadata": {"filename": "empty.txt", "page_count": 0}, "pages": []})JSON";

// ============================================================
// Test Fixture
// ============================================================

class SPCPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Deterministic per-deployment SipHash key so the assembler's
        // HashChildIdToBlockId(child_id) mints stable, non-zero block ids.
        id::SetDeploymentHashKeyForTesting({0x0123456789abcdefULL, 0xfedcba9876543210ULL});

        tmp_root_ = std::filesystem::temp_directory_path() /
                    ("spc_pipe_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(tmp_root_);
        config_.data_root = tmp_root_.string();

        // Stand up the pool + admit a single resident namespace, then acquire a façade.
        std::filesystem::create_directories(tmp_root_ / "test-ns-u");  // store.db / pending.wal home
        pool_ = MakePool();
        ASSERT_TRUE(pool_->AdmitCreate("test-ns", 100).ok());
        ASSERT_NE(fake_index_, nullptr) << "index factory Open did not run during admit";

        facade_ = std::make_unique<resource::NamespaceFacade>(*pool_, "test-ns");
        ASSERT_TRUE(facade_->Acquire().ok());

        // F06 factory with a *mock* Docling bridge that emits the multi-paragraph
        // payload (default). RebuildFactory() lets a test swap the payload.
        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        assembler_ = std::make_unique<BlockAssembler>();
        chunker_ = std::make_unique<cortrix::chunker::ParentChildChunker>(
            cortrix::chunker::ChunkerConfig{});
        RebuildFactory(kDoclingMultiPara);

        // Create temp test files (real on-disk so the F06 factory pre-check stats them).
        txt_path_ = WriteTmp(".txt", "real text on disk\n");
        md_path_ = WriteTmp(".md", "# Title\n\ncontent\n");
    }

    void TearDown() override {
        for (const auto& p : tmp_files_) std::remove(p.c_str());
        facade_.reset();
        pool_.reset();
        std::error_code ec;
        std::filesystem::remove_all(tmp_root_, ec);
    }

    // Write a throwaway file under /tmp with the given suffix; tracked for cleanup.
    std::string WriteTmp(const std::string& suffix, const std::string& content) {
        char tmpl[] = "/tmp/cortrix_spcp_XXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd >= 0) ::close(fd);
        std::string path = std::string(tmpl) + suffix;
        std::rename(tmpl, path.c_str());
        std::ofstream f(path);
        f << content;
        f.close();
        tmp_files_.push_back(path);
        return path;
    }

    // Build a mock python bridge that prints `json_literal`, and rebuild the F06
    // factory + pipeline around it (primary = real DoclingParser over the mock).
    void RebuildFactory(const std::string& docling_json) {
        std::string script = WriteTmp(".py",
            "import sys\nprint('''" + docling_json + "''')\n");
        cortrix::spc::ParserFactoryConfig cfg;
        factory_ = std::make_unique<cortrix::spc::DocumentParserFactory>(cfg);
        cortrix::spc::DoclingParserConfig dc;
        dc.python_path = "python3";
        dc.script_path = script;
        factory_->SetPrimaryParser(std::make_unique<cortrix::spc::DoclingParser>(dc));
        cortrix::spc::PaddleOCRParserConfig pc;
        pc.python_path = "python3";
        pc.script_path = script;  // same mock for fallback (not exercised in these tests)
        factory_->SetFallbackParser(std::make_unique<cortrix::spc::PaddleOCRParser>(pc));
        pipeline_ = std::make_unique<SPCPipeline>(
            *factory_, *chunker_, *embedder_, *assembler_, enricher_);
    }

    // A UnitDescriptor whose unit_id drives the per-Unit temp dir layout.
    cortrix::catalog::UnitDescriptor Unit(const std::string& unit_id) {
        cortrix::catalog::UnitDescriptor u;
        u.unit_id = unit_id;
        u.tenant_id = "t1";
        return u;
    }

    // Real WriteCoordinator factory (C1: the pool resolves + passes the 3 stores).
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

    // GetActiveUnit -> Unit(ns+"-u"); index Open -> a FakeIndex captured into
    // fake_index_ so tests can assert the vector write hit it.
    std::unique_ptr<resource::DefaultNamespacePool> MakePool() {
        ON_CALL(ns_router_, GetActiveUnit(_))
            .WillByDefault(Invoke([this](const std::string& ns) {
                return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
            }));
        ON_CALL(index_factory_, Open(_))
            .WillByDefault(Invoke([this](const std::string&) {
                auto idx = std::make_unique<FakeIndex>(0);
                fake_index_ = idx.get();
                return Result<std::unique_ptr<cortrix::store::IIndex>>(std::move(idx));
            }));
        return std::make_unique<resource::DefaultNamespacePool>(
            &index_factory_, MakeRealCoordFactory(), &ns_router_, &unit_router_, config_);
    }

    // Create a doc on the façade's real store and return its (D-I6) ULID.
    std::string CreateDoc(const std::string& source_path = "/spc/doc.txt") {
        cortrix::CortrixDoc doc;
        doc.source_type = "test";
        doc.source_path = source_path;
        EXPECT_EQ(facade_->store().doc_create(doc), 0);
        EXPECT_FALSE(doc.doc_id.empty());
        return doc.doc_id;
    }

    // Blocks persisted for a doc, read back from real SQLite (ordered by chunk_index).
    std::vector<cortrix::CortrixBlock> BlocksOf(const std::string& doc_id) {
        std::vector<cortrix::CortrixBlock> out;
        EXPECT_EQ(facade_->store().block_get_by_doc(doc_id, out), 0);
        return out;
    }

    DocStatus StatusOf(const std::string& doc_id) {
        cortrix::CortrixDoc d;
        EXPECT_EQ(facade_->store().doc_get(doc_id, d), 0);
        return d.status;
    }

    int64_t TotalBlocks() {
        int64_t n = 0;
        EXPECT_EQ(facade_->store().block_count(&n), 0);
        return n;
    }

    // The concrete SQLite store behind the facade, for the SetFailNextOps testing
    // seam (F23 section 4.5): arming it makes the next N public CRUD ops return -1, which
    // is the only way to reach the pipeline's store-write failure / rollback
    // branches against an otherwise-real store.
    cortrix::CortrixStoreSqlite* ConcreteStore() {
        return dynamic_cast<cortrix::CortrixStoreSqlite*>(&facade_->store());
    }

    void InitTask(SPCTask& task,
                  const std::string& source_path,
                  const std::string& mime_type,
                  const std::string& doc_id) {
        task.task_id = 1;
        task.doc_id = doc_id;
        task.namespace_name = "test-ns";
        task.source_path = source_path;
        task.mime_type = mime_type;
        task.priority = SPCPriority::kP0;
        task.stage = SPCStage::kQueued;
        task.cancelled.store(false);
    }

    std::unique_ptr<SPCTask> MakeTask(const std::string& source_path,
                                       const std::string& mime_type,
                                       const std::string& doc_id) {
        auto t = std::make_unique<SPCTask>();
        InitTask(*t, source_path, mime_type, doc_id);
        return t;
    }

    // ── F05 pool plumbing ──
    std::filesystem::path tmp_root_;
    resource::F05Config config_;
    NiceMock<MockIndexFactory> index_factory_;
    NiceMock<MockNSRouter> ns_router_;
    NiceMock<MockUnitRouter> unit_router_;
    FakeIndex* fake_index_ = nullptr;  // captured at admit time
    std::unique_ptr<resource::DefaultNamespacePool> pool_;
    std::unique_ptr<resource::NamespaceFacade> facade_;

    // ── real pipeline (F06 parser + F34 chunker) ──
    std::unique_ptr<cortrix::spc::DocumentParserFactory> factory_;
    std::unique_ptr<cortrix::chunker::ParentChildChunker> chunker_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<BlockAssembler> assembler_;
    cortrix::spc::NullEnricher enricher_;  // F03 short-circuit (lifetime ≥ pipeline_)
    std::unique_ptr<SPCPipeline> pipeline_;

    std::vector<std::string> tmp_files_;
    std::string txt_path_;
    std::string md_path_;
};

// ============================================================
// Stage orchestration tests
// ============================================================

TEST_F(SPCPipelineTest, ProcessTxtFile_FullPipeline) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(task.stage, SPCStage::kDone);
    EXPECT_TRUE(task.error_message.empty());

    // Child blocks landed in real SQLite + vectors landed in the captured index, 1:1.
    auto blocks = BlocksOf(doc_id);
    EXPECT_GT(blocks.size(), 0u);
    EXPECT_GT(fake_index_->added_ids().size(), 0u);
    EXPECT_EQ(blocks.size(), fake_index_->added_ids().size());
}

// [A unified-blocks] children land as `blocks` rows with a non-empty child_id +
// parent_id; the parents land in the `parents` table (verified via parent_get).
TEST_F(SPCPipelineTest, UnifiedWrite_ParentsAndChildrenPersisted) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    ASSERT_EQ(rc, 0) << task.error_message;
    EXPECT_EQ(task.stage, SPCStage::kDone);

    auto blocks = BlocksOf(doc_id);
    ASSERT_GT(blocks.size(), 0u);

    // Under A unified-blocks the `blocks` table is heterogeneous: child rows
    // (block_type = source modality kBlockFile, child-ness carried by child_id) plus
    // one doc-level F08 META row (block_type = kBlockMeta, no child_id). Separate them.
    std::vector<std::string> parent_ids;
    int meta_count = 0;
    for (const auto& b : blocks) {
        EXPECT_EQ(b.doc_id, doc_id);
        if (b.block_type == static_cast<int>(kBlockMeta)) {
            ++meta_count;
            EXPECT_TRUE(b.child_id.empty()) << "META is doc-level, not a child";
            continue;
        }
        EXPECT_FALSE(b.child_id.empty()) << "child row must carry a child_id";
        EXPECT_FALSE(b.parent_id.empty()) << "non-flat child must carry a parent_id";
        EXPECT_EQ(b.block_type, static_cast<int>(kBlockFile));
        parent_ids.push_back(b.parent_id);
    }
    EXPECT_EQ(meta_count, 1) << "exactly one F08 META block per doc";

    // Each referenced parent_id resolves to a real row in the `parents` table.
    ASSERT_FALSE(parent_ids.empty());
    for (const auto& pid : parent_ids) {
        cortrix::CortrixParent p;
        int prc = facade_->store().parent_get(pid, p);
        ASSERT_EQ(prc, 0) << "parent_get failed for " << pid;
        EXPECT_EQ(p.parent_id, pid);
        EXPECT_EQ(p.doc_id, doc_id);
        EXPECT_EQ(p.namespace_id, facade_->namespace_id());
        EXPECT_FALSE(p.parent_text.empty());
    }
}

// [A unified-blocks F08] After chunking (D6 lock F06→F34→F08) one doc-level Metadata
// Block (block_type = kBlockMeta = 8) is written into the same `blocks` table. It
// carries no child_id/parent_id but has a natural-language block_text (content_text)
// + the 26-field metadata_json; exactly one per doc (idx_blocks_meta_doc).
TEST_F(SPCPipelineTest, F08MetadataBlock_Persisted) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    ASSERT_EQ(rc, 0) << task.error_message;

    int meta_count = 0;
    for (const auto& b : BlocksOf(doc_id)) {
        if (b.block_type != static_cast<int>(kBlockMeta)) continue;
        ++meta_count;
        EXPECT_TRUE(b.child_id.empty());
        EXPECT_TRUE(b.parent_id.empty());
        EXPECT_FALSE(b.content_text.empty()) << "META block_text (embedding input)";
        EXPECT_FALSE(b.metadata_json.empty()) << "META 26-field metadata_json";
        EXPECT_NE(b.metadata_json.find(doc_id), std::string::npos)
            << "metadata_json should carry the doc_id";
    }
    EXPECT_EQ(meta_count, 1) << "exactly one F08 META block per doc (idx_blocks_meta_doc)";
}

// [A unified-blocks F10] The SPC pipeline runs F10 dedup (exact + semantic) over the
// children before the write. Two byte-identical large paragraphs (each > parent_size
// so each forms its own parent → byte-identical child chunks) must collapse: no two
// written child rows share identical text.
TEST_F(SPCPipelineTest, F10_DedupRemovesDuplicateChildren) {
    const std::string para(4400, 'x');  // ~1100 tokens (> parent_size 1024) → own parent
    const std::string json =
        std::string(R"JSON({"status":0,"parser":"docling",)JSON") +
        R"JSON("metadata":{"filename":"dup.txt","page_count":1,"doc_language":"en"},)JSON" +
        R"JSON("pages":[{"page_num":1,"page_text":"x","page_metadata":{"page_num":1,)JSON" +
        R"JSON("parser_used":"docling","page_confidence":0.95,"is_scan_page":false,"char_count":1},)JSON" +
        R"JSON("paragraphs":[{"text":")JSON" + para +
        R"JSON(","page":1,"type":"TEXT","confidence":0.95,"language":"en"},)JSON" +
        R"JSON({"text":")JSON" + para +
        R"JSON(","page":1,"type":"TEXT","confidence":0.95,"language":"en"}]}]})JSON";
    RebuildFactory(json);

    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    int rc = pipeline_->Process(task, *facade_);
    ASSERT_EQ(rc, 0) << task.error_message;

    std::vector<std::string> texts;
    for (const auto& b : BlocksOf(doc_id)) {
        if (b.block_type == static_cast<int>(kBlockFile)) texts.push_back(b.content_text);
    }
    ASSERT_GT(texts.size(), 0u);
    for (size_t i = 0; i < texts.size(); ++i)
        for (size_t j = i + 1; j < texts.size(); ++j)
            EXPECT_NE(texts[i], texts[j])
                << "F10 dedup: no two surviving child rows may share identical text";
}

TEST_F(SPCPipelineTest, ProcessMarkdownFile_Parses) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(md_path_, "text/markdown", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0) << task.error_message;
    EXPECT_EQ(task.stage, SPCStage::kDone);

    EXPECT_GE(BlocksOf(doc_id).size(), 1u);
}

TEST_F(SPCPipelineTest, ParseError_FileNotFound) {
    // F06 factory pre-check: a non-existent file → FILE_NOT_FOUND (parse error).
    auto task_uptr_ = MakeTask("/nonexistent/file.txt", "text/plain",
                               "01JTESTDOC0000000000000042");
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kError);
    EXPECT_NE(task.error_message.find("parse failed"), std::string::npos);
}

TEST_F(SPCPipelineTest, CancelledBeforeParsing) {
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", "01JTESTDOC0000000000000042");
    SPCTask& task = *task_uptr_;
    task.cancelled.store(true);

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kCancelled);

    EXPECT_EQ(TotalBlocks(), 0);
}

TEST_F(SPCPipelineTest, VectorIndexAddFailure_Error) {
    fake_index_->set_add_should_fail(true);

    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kError);
    // C2: the vector write goes through facade.vec_index().AddPoints; on failure the
    // pipeline rolls the txn back and surfaces "AddPoints: <reason>".
    EXPECT_NE(task.error_message.find("AddPoints"), std::string::npos);
    // Rollback before any block_insert / parent_insert → nothing persisted.
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);
}

TEST_F(SPCPipelineTest, EmptyDoc_DoneWithZeroBlocks) {
    // Docling returns zero pages → chunker EMPTY_DOCUMENT → ready with 0 blocks.
    RebuildFactory(kDoclingEmpty);

    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0) << task.error_message;
    EXPECT_EQ(task.stage, SPCStage::kDone);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);
    EXPECT_EQ(StatusOf(doc_id), DocStatus::kReady);
}

TEST_F(SPCPipelineTest, StageProgression_VerifyStages) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    EXPECT_EQ(task.stage, SPCStage::kQueued);

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(task.stage, SPCStage::kDone);
}

TEST_F(SPCPipelineTest, BlockMetadata_CorrectDocId) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);

    auto blocks = BlocksOf(doc_id);
    EXPECT_GT(blocks.size(), 0u);
    for (const auto& block : blocks) {
        EXPECT_EQ(block.doc_id, doc_id);
    }
}

TEST_F(SPCPipelineTest, BlockType_InferredFromMime) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);

    for (const auto& block : BlocksOf(doc_id)) {
        // Child rows carry the inferred source modality; the F08 META row is kBlockMeta.
        if (block.child_id.empty()) {
            EXPECT_EQ(block.block_type, static_cast<int>(kBlockMeta));
        } else {
            EXPECT_EQ(block.block_type, static_cast<int>(kBlockFile));
        }
    }
}

TEST_F(SPCPipelineTest, ChunkIndicesAreSequential) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);

    // Child blocks carry sequential 0-based chunk indices; the F08 META row
    // (block_type kBlockMeta) is doc-level and not part of the child sequence.
    auto blocks = BlocksOf(doc_id);
    int seq = 0;
    for (const auto& b : blocks) {
        if (b.block_type == static_cast<int>(kBlockMeta)) continue;
        EXPECT_EQ(b.chunk_index, seq++);
    }
    EXPECT_GT(seq, 0);
}

// ============================================================
// Processing Level Tests (D3 design alignment)
// ============================================================

TEST_F(SPCPipelineTest, L0_SkipEntireProcessing) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 0;  // L0

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(task.stage, SPCStage::kDone);

    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);
    EXPECT_EQ(fake_index_->added_ids().size(), 0u);
    EXPECT_EQ(StatusOf(doc_id), DocStatus::kReady);
}

TEST_F(SPCPipelineTest, L1_MetadataOnlyBlock) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 1;  // L1

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(task.stage, SPCStage::kDone);

    auto blocks = BlocksOf(doc_id);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].processing_level, 1);
    EXPECT_EQ(blocks[0].chunk_index, 0);
    EXPECT_EQ(fake_index_->added_ids().size(), 0u);
    EXPECT_EQ(StatusOf(doc_id), DocStatus::kReady);
}

TEST_F(SPCPipelineTest, L2_ChunksButNoEmbedding) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 2;  // L2

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(task.stage, SPCStage::kDone);

    EXPECT_GT(BlocksOf(doc_id).size(), 0u);
    EXPECT_EQ(fake_index_->added_ids().size(), 0u);  // L2 skips Embedder
    EXPECT_EQ(StatusOf(doc_id), DocStatus::kReady);
}

// [A unified-blocks] parents are written regardless of level (L2 has no embedding
// but still emits parent rows).
TEST_F(SPCPipelineTest, L2_ParentsStillPersisted) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 2;

    int rc = pipeline_->Process(task, *facade_);
    ASSERT_EQ(rc, 0) << task.error_message;

    auto blocks = BlocksOf(doc_id);
    ASSERT_GT(blocks.size(), 0u);
    // Pick a child block (the F08 META row has no parent_id) and resolve its parent.
    std::string a_parent_id;
    for (const auto& b : blocks) {
        if (!b.parent_id.empty()) { a_parent_id = b.parent_id; break; }
    }
    ASSERT_FALSE(a_parent_id.empty()) << "expected a child block with a parent_id";
    cortrix::CortrixParent p;
    ASSERT_EQ(facade_->store().parent_get(a_parent_id, p), 0);
    EXPECT_EQ(p.doc_id, doc_id);
}

TEST_F(SPCPipelineTest, L3_FullPipelineWithVectors) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 3;  // L3 (default)

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(task.stage, SPCStage::kDone);

    auto blocks = BlocksOf(doc_id);
    EXPECT_GT(blocks.size(), 0u);
    EXPECT_GT(fake_index_->added_ids().size(), 0u);
    EXPECT_EQ(blocks.size(), fake_index_->added_ids().size());
}

TEST_F(SPCPipelineTest, L1_CancelledBeforeAssembly) {
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", "01JTESTDOC0000000000000042");
    SPCTask& task = *task_uptr_;
    task.processing_level = 1;
    task.cancelled.store(true);  // Pre-cancelled

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kCancelled);
    EXPECT_EQ(TotalBlocks(), 0);
}

TEST_F(SPCPipelineTest, DefaultProcessingLevel_IsL3) {
    // Default processing_level in SPCTask should be 3 (L3)
    SPCTask task;
    EXPECT_EQ(task.processing_level, 3);
}

// ============================================================
// Memory-session path (Inc 4-3 item 6: bypass parse + chunker)
// ============================================================

TEST_F(SPCPipelineTest, MemorySession_WritesOneFlatBlock) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask("/n/a", "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.source_type = "memory_session";
    task.content_hash = "what is X?\n---\nX is Y.";
    task.metadata_json = R"({"interaction_id":"int-001","turn":5,"ttl_seconds":86400})";

    int rc = pipeline_->Process(task, *facade_);
    ASSERT_EQ(rc, 0) << task.error_message;
    EXPECT_EQ(task.stage, SPCStage::kDone);

    auto blocks = BlocksOf(doc_id);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].block_type, static_cast<int>(kBlockMemory));
    EXPECT_TRUE(blocks[0].child_id.empty()) << "memory block is flat (no child_id)";
    EXPECT_TRUE(blocks[0].parent_id.empty());
    EXPECT_EQ(fake_index_->added_ids().size(), 1u);  // L3 embedded + indexed

    // No parent row for a memory doc.
    cortrix::CortrixParent p;
    EXPECT_NE(facade_->store().parent_get(blocks[0].parent_id, p), 0);

    // task.metadata_json flows into the memory block's BLOB payload.
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blocks[0].data.data(), blocks[0].data.size(), &hdr));
    ASSERT_GT(hdr->metadata_length, 0u);
    size_t meta_start = 128 + hdr->metadata_offset;
    std::string extracted_meta(
        reinterpret_cast<const char*>(blocks[0].data.data() + meta_start),
        hdr->metadata_length);
    EXPECT_EQ(extracted_meta, task.metadata_json);
}

// ============================================================
// SPCRouter Tests (D3 design alignment) — pure static, façade-independent
// ============================================================

TEST_F(SPCPipelineTest, Router_InferProcessingLevel_TempFile_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/x-temp"), 0);
}

TEST_F(SPCPipelineTest, Router_InferProcessingLevel_Executable_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/x-executable"), 0);
}

TEST_F(SPCPipelineTest, Router_InferProcessingLevel_Font_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("font/ttf"), 0);
}

TEST_F(SPCPipelineTest, Router_InferProcessingLevel_Pdf_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/pdf"), 3);
}

TEST_F(SPCPipelineTest, Router_InferProcessingLevel_TextPlain_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("text/plain"), 3);
}

TEST_F(SPCPipelineTest, Router_InferProcessingLevel_ImagePng_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("image/png"), 3);
}

TEST_F(SPCPipelineTest, Router_InferMimeType_Csv) {
    EXPECT_EQ(SPCRouter::InferMimeType("data.csv"), "text/csv");
}

TEST_F(SPCPipelineTest, Router_InferMimeType_Html) {
    EXPECT_EQ(SPCRouter::InferMimeType("page.html"), "text/html");
}

TEST_F(SPCPipelineTest, Router_InferMimeType_Json) {
    EXPECT_EQ(SPCRouter::InferMimeType("config.json"), "application/json");
}

TEST_F(SPCPipelineTest, Router_InferMimeType_Xml) {
    EXPECT_EQ(SPCRouter::InferMimeType("data.xml"), "application/xml");
}

TEST_F(SPCPipelineTest, Router_InferMimeType_TmpFile) {
    EXPECT_EQ(SPCRouter::InferMimeType("cache.tmp"), "application/x-temp");
}

TEST_F(SPCPipelineTest, Router_InferMimeType_ExeFile) {
    EXPECT_EQ(SPCRouter::InferMimeType("program.exe"), "application/x-executable");
}

TEST_F(SPCPipelineTest, Router_InferMimeType_FontFile) {
    EXPECT_EQ(SPCRouter::InferMimeType("arial.ttf"), "font/ttf");
}

TEST_F(SPCPipelineTest, Router_IsSupported_Csv) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/csv"));
}

TEST_F(SPCPipelineTest, Router_IsSupported_Html) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/html"));
}

TEST_F(SPCPipelineTest, Router_IsSupported_Json) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/json"));
}

TEST_F(SPCPipelineTest, Router_IsSupported_Xml) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/xml"));
}

TEST_F(SPCPipelineTest, Router_IsSupported_TextXml) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/xml"));
}

TEST_F(SPCPipelineTest, Router_IsSupported_TempFiles_NotSupported) {
    EXPECT_FALSE(SPCRouter::IsSupported("application/x-temp"));
    EXPECT_FALSE(SPCRouter::IsSupported("application/x-executable"));
    EXPECT_FALSE(SPCRouter::IsSupported("font/ttf"));
}

// ============================================================
// L2 cancellation
// ============================================================

TEST_F(SPCPipelineTest, L2_CancelledBeforeChunking) {
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", "01JTESTDOC0000000000000042");
    SPCTask& task = *task_uptr_;
    task.processing_level = 2;
    task.cancelled.store(true);

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kCancelled);
}

TEST_F(SPCPipelineTest, L3_CancelledBeforeAssembly) {
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", "01JTESTDOC0000000000000042");
    SPCTask& task = *task_uptr_;
    task.processing_level = 3;
    task.cancelled.store(true);  // Pre-cancel to hit first cancel check

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kCancelled);
}

// ============================================================
// DocStatus updated on various outcomes
// ============================================================

TEST_F(SPCPipelineTest, DocStatus_ReadyOnSuccess) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;

    pipeline_->Process(task, *facade_);
    EXPECT_EQ(StatusOf(doc_id), DocStatus::kReady);
}

TEST_F(SPCPipelineTest, L0_DocStatus_Ready) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 0;

    pipeline_->Process(task, *facade_);
    EXPECT_EQ(StatusOf(doc_id), DocStatus::kReady);
}

// ============================================================
// Router tests that were in pipeline fixture
// ============================================================

TEST_F(SPCPipelineTest, Router_InferMimeType_UpperCase) {
    EXPECT_EQ(SPCRouter::InferMimeType("REPORT.PDF"), "application/pdf");
}

TEST_F(SPCPipelineTest, Router_NeedsOcr_PdfWithText_No) {
    EXPECT_FALSE(SPCRouter::NeedsOcr("application/pdf", true));
}

TEST_F(SPCPipelineTest, Router_NeedsOcr_PdfNoText_Yes) {
    EXPECT_TRUE(SPCRouter::NeedsOcr("application/pdf", false));
}

TEST_F(SPCPipelineTest, Router_NeedsOcr_ImageAlways_Yes) {
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/png", true));
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/jpeg", false));
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/tiff", true));
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/bmp", false));
}

TEST_F(SPCPipelineTest, Router_NeedsOcr_Text_No) {
    EXPECT_FALSE(SPCRouter::NeedsOcr("text/plain", false));
    EXPECT_FALSE(SPCRouter::NeedsOcr("text/markdown", true));
}

// ============================================================
// L3 embedding stage reached (fallback random embeddings → vectors added)
// ============================================================

TEST_F(SPCPipelineTest, L3_EmbeddingStage_Reached) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 3;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(task.stage, SPCStage::kDone);
    EXPECT_GT(fake_index_->added_ids().size(), 0u);
}

TEST_F(SPCPipelineTest, L3_PreCancelled_HitsFirstCheck) {
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", "01JTESTDOC0000000000000042");
    SPCTask& task = *task_uptr_;
    task.processing_level = 3;
    task.cancelled.store(true);

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kCancelled);
    EXPECT_EQ(TotalBlocks(), 0);
    EXPECT_EQ(fake_index_->added_ids().size(), 0u);
}

// ============================================================
// L2 assembly stage reached and blocks persisted in order
// ============================================================

TEST_F(SPCPipelineTest, L2_AssemblyStage_VerifiedByBlockInsert) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 2;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(task.stage, SPCStage::kDone);

    // Child blocks carry sequential 0-based chunk indices; the F08 META row
    // (block_type kBlockMeta) is doc-level and not part of the child sequence.
    auto blocks = BlocksOf(doc_id);
    int seq = 0;
    for (const auto& b : blocks) {
        if (b.block_type == static_cast<int>(kBlockMeta)) continue;
        EXPECT_EQ(b.chunk_index, seq++);
    }
    EXPECT_GT(seq, 0);
}

// ============================================================
// vec_index AddPoints failure at L3 (C2 rollback path)
// ============================================================

TEST_F(SPCPipelineTest, L3_VecAddFailure_ProducesCorrectError) {
    fake_index_->set_add_should_fail(true);

    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 3;

    int rc = pipeline_->Process(task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task.stage, SPCStage::kError);
    EXPECT_NE(task.error_message.find("AddPoints"), std::string::npos);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);  // rolled back
}

// ============================================================
// C2 §9.4 rollback cleanup is in-process. A mid-write Rollback must wipe the
// already-inserted SQLite blocks AND parents via the pool's rollback callback
// (WC::Rollback -> rollback_cb_ -> MarkDelete vectors + DELETE FROM blocks WHERE
// block_id IN … + DELETE FROM parents WHERE doc_id = ?). Driven directly through
// the façade's WriteCoordinator (block_insert/parent_insert can't be forced to
// fail against real SQLite in external-conn mode).
// ============================================================

TEST_F(SPCPipelineTest, RollbackCallbackDeletesInsertedBlocksInProcess) {
    std::string doc_id = CreateDoc();

    std::vector<cortrix::BlockId> block_ids;
    std::vector<cortrix::CortrixBlock> blocks;
    for (int i = 0; i < 3; ++i) {
        cortrix::CortrixBlock b;
        b.doc_id = doc_id;
        b.chunk_index = i;
        b.block_type = static_cast<int>(kBlockFile);
        b.processing_level = 3;
        b.block_id = id::HashChildIdToBlockId(id::GenerateUlid());
        b.data = BlockBuild(kBlockFile, kLevelL3, "blk", "", "", 0, 0);
        b.content_text = "blk";
        block_ids.push_back(b.block_id);
        blocks.push_back(std::move(b));
    }

    store::TxnHandle txn;
    ASSERT_TRUE(facade_->write_coordinator()
                    .BeginWrite(doc_id, block_ids, /*writes_blob=*/false, &txn)
                    .ok());
    for (auto& b : blocks) ASSERT_EQ(facade_->store().block_insert(b), 0);
    ASSERT_EQ(BlocksOf(doc_id).size(), 3u);  // really persisted in SQLite

    // Rollback runs the pool callback in-process — NOT a recovery-only sweep.
    ASSERT_TRUE(facade_->write_coordinator().Rollback(txn).ok());
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u)
        << "rollback callback must delete the inserted blocks in-process";
}

// [A unified-blocks Inc 4-3] The rollback callback also drops the doc's parents.
TEST_F(SPCPipelineTest, RollbackCallbackDeletesInsertedParentsInProcess) {
    std::string doc_id = CreateDoc();

    cortrix::CortrixBlock b;
    b.doc_id = doc_id;
    b.chunk_index = 0;
    b.block_type = static_cast<int>(kBlockFile);
    b.processing_level = 3;
    b.block_id = id::HashChildIdToBlockId(id::GenerateUlid());
    b.data = BlockBuild(kBlockFile, kLevelL3, "blk", "", "", 0, 0);
    b.content_text = "blk";
    b.child_id = id::GenerateUlid();

    cortrix::CortrixParent p;
    p.parent_id = id::GenerateUlid();
    p.doc_id = doc_id;
    p.namespace_id = facade_->namespace_id();
    p.parent_text = "parent body";
    p.metadata_json = "{}";

    store::TxnHandle txn;
    ASSERT_TRUE(facade_->write_coordinator()
                    .BeginWrite(doc_id, {b.block_id}, /*writes_blob=*/false, &txn)
                    .ok());
    ASSERT_EQ(facade_->store().parent_insert(p), 0);
    ASSERT_EQ(facade_->store().block_insert(b), 0);

    cortrix::CortrixParent got;
    ASSERT_EQ(facade_->store().parent_get(p.parent_id, got), 0);  // really persisted

    ASSERT_TRUE(facade_->write_coordinator().Rollback(txn).ok());
    EXPECT_NE(facade_->store().parent_get(p.parent_id, got), 0)
        << "rollback callback must delete the inserted parents in-process";
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);
}

// ============================================================
// metadata_json flow — child blocks carry the F34-inherited DocumentMetadata
// (NOT task.metadata_json; that path is the F06/F34 doc-meta inheritance). The
// memory path (above) is the one that carries task.metadata_json.
// ============================================================

TEST_F(SPCPipelineTest, ChildBlocks_CarryInheritedDocMetadata) {
    std::string doc_id = CreateDoc();
    auto task_uptr_ = MakeTask(txt_path_, "text/plain", doc_id);
    SPCTask& task = *task_uptr_;
    task.processing_level = 3;

    int rc = pipeline_->Process(task, *facade_);
    ASSERT_EQ(rc, 0) << task.error_message;

    auto blocks = BlocksOf(doc_id);
    ASSERT_GT(blocks.size(), 0u);
    for (const auto& block : blocks) {
        // The F08 META row carries the 26-field F08 schema (lang/parser/counts…),
        // not the child-inherited DocumentMetadata (doc_language). This test is about
        // child blocks, so skip the doc-level META row.
        if (block.block_type == static_cast<int>(kBlockMeta)) continue;
        // metadata_json column carries the inherited per-child DocumentMetadata.
        EXPECT_FALSE(block.metadata_json.empty());
        // The BLOB payload embeds the same metadata.
        const cortrix_block_header_t* hdr = nullptr;
        ASSERT_TRUE(BlockParse(block.data.data(), block.data.size(), &hdr));
        EXPECT_GT(hdr->metadata_length, 0u);
        // doc-level fields inherited from F06 metadata (filename/doc_language).
        EXPECT_NE(block.metadata_json.find("doc_language"), std::string::npos);
    }
}

// ============================================================
// F03 enrich integration (③b): an available LlmEnricher feeds Stage 3.6
// (EnrichBatch) and the write phase persists enriched_score + entities and folds
// the summary into block metadata_json (F03 §3.1), all on the real store handle.
// ============================================================

TEST_F(SPCPipelineTest, LlmEnricherPersistsEnrichmentAndSummary) {
    using ::testing::Return;

    // MockLlmClient answers every batch with the F03 index-keyed JSON shape
    // ({"0":{entities,summary,score}, ...}) for indices 0..7, so each child in a
    // (≤ batch_size=8) batch gets a real enrichment. WillRepeatedly → any batch count.
    auto mock = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse ok;
    ok.status = Status::Ok();
    ok.finish_reason = "stop";
    ok.prompt_tokens = 20;
    ok.completion_tokens = 8;
    ok.model = "gpt-4o-mini";
    {
        nlohmann::json batch;
        for (int i = 0; i < 8; ++i) {
            batch[std::to_string(i)] = {
                {"entities", nlohmann::json::array({{{"text", "Cortrix"},
                                                     {"type", "PRODUCT"},
                                                     {"start_offset", 0},
                                                     {"end_offset", 7}}})},
                {"summary", "enriched summary"},
                {"score", 0.77},
            };
        }
        ok.content = batch.dump();
    }
    EXPECT_CALL(*mock, Chat(_, _)).WillRepeatedly(Return(ok));

    spc::EnricherConfig cfg;
    cfg.type = spc::EnricherType::kLlm;
    cfg.enabled = true;
    cfg.model = "gpt-4o-mini";
    cfg.prompt_template_id = "default-zh";
    spc::LlmEnricher enricher(cfg, mock);
    ASSERT_TRUE(enricher.IsAvailable());

    // A pipeline wired to the available enricher (the fixture's pipeline_ uses the
    // NullEnricher). Reuses the fixture's real parser/chunker/embedder/assembler.
    SPCPipeline enr_pipeline(*factory_, *chunker_, *embedder_, *assembler_, enricher);

    std::string doc_id = CreateDoc("/spc/enrich.txt");
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    task_uptr->processing_level = 3;  // L3 (full vector path)

    int rc = enr_pipeline.Process(*task_uptr, *facade_);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(task_uptr->stage, SPCStage::kDone);

    // Read back through the real store handle (X1).
    sqlite3* db = facade_->store().db_handle();
    ASSERT_NE(db, nullptr);
    auto CountOf = [&](const char* sql) -> int {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, nullptr), SQLITE_OK);
        int n = 0;
        if (st && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return n;
    };

    // enriched_score persisted on the child blocks (META / non-child blocks stay NULL).
    EXPECT_GT(CountOf("SELECT COUNT(*) FROM blocks WHERE enriched_score > 0"), 0);
    // entities written + FTS5-indexed.
    EXPECT_GT(CountOf("SELECT COUNT(*) FROM entities"), 0);
    EXPECT_GT(CountOf("SELECT COUNT(*) FROM entities_fts WHERE entities_fts MATCH 'Cortrix'"), 0);

    // ④ F07 (option A — Matrix level owns block-level processing_level + semantic_score).
    // docling parser (level 2) + LlmEnricher (level 4) → child Matrix level 4 / score 1.0;
    // the META block is locked to level 0 / score 0.2 (ARCH §5.2.1). ABS tolerance because a
    // 0.2f/0.6f REAL widens to double imprecisely (1.0f is exact, but ABS is harmless).
    EXPECT_GT(CountOf("SELECT COUNT(*) FROM blocks WHERE semantic_score IS NOT NULL"), 0);
    EXPECT_GT(CountOf("SELECT COUNT(*) FROM blocks WHERE child_id IS NOT NULL AND child_id != '' "
                      "AND processing_level = 4 AND ABS(semantic_score - 1.0) < 0.001"), 0);
    EXPECT_EQ(CountOf("SELECT COUNT(*) FROM blocks WHERE block_type = 8 "
                      "AND processing_level = 0 AND ABS(semantic_score - 0.2) < 0.001"), 1);

    // Summary folded into a child block's metadata_json (F03 §3.1).
    bool found_summary = false;
    for (const auto& b : BlocksOf(doc_id)) {
        if (!b.child_id.empty() &&
            b.metadata_json.find("enriched summary") != std::string::npos) {
            found_summary = true;
            break;
        }
    }
    EXPECT_TRUE(found_summary);
}

// NullEnricher (the fixture default) leaves no enrichment: the slot is wired but
// short-circuits — guards the ③a "behavior unchanged" contract at the SQL level.
TEST_F(SPCPipelineTest, NullEnricherWritesNoEnrichment) {
    std::string doc_id = CreateDoc("/spc/noenrich.txt");
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    int rc = pipeline_->Process(*task_uptr, *facade_);
    ASSERT_EQ(rc, 0);

    sqlite3* db = facade_->store().db_handle();
    ASSERT_NE(db, nullptr);
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM blocks WHERE enriched_score IS NOT NULL", -1, &st, nullptr),
        SQLITE_OK);
    int enriched = 0;
    if (sqlite3_step(st) == SQLITE_ROW) enriched = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    EXPECT_EQ(enriched, 0);

    // ④ F07 still runs without enrichment (it is independent of F03): docling parser
    // (level 2) + NullEnricher (level 0) → child Matrix level 2 / semantic_score 0.6.
    sqlite3_stmt* st2 = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM blocks WHERE child_id IS NOT NULL AND child_id != '' "
            "AND processing_level = 2 AND ABS(semantic_score - 0.6) < 0.001",
        -1, &st2, nullptr), SQLITE_OK);
    int scored = 0;
    if (sqlite3_step(st2) == SQLITE_ROW) scored = sqlite3_column_int(st2, 0);
    sqlite3_finalize(st2);
    EXPECT_GT(scored, 0);
}

// ============================================================
// ⑤c — OnDocumentWritten doc-summary enqueue seam (gate)
// ============================================================

TEST_F(SPCPipelineTest, OnDocumentWrittenEnqueuesDocSummaryWhenSeamInstalled) {
    std::vector<async::SubmitRequest> captured;
    pipeline_->SetDocSummaryEnqueue(
        [&](const async::SubmitRequest& r) { captured.push_back(r); });
    pipeline_->OnDocumentWritten("doc-abc", "ns-1");
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].doc_id, "doc-abc");
    EXPECT_EQ(captured[0].namespace_id, "ns-1");
    EXPECT_EQ(captured[0].task_type, async::kTaskDocSummary);
    EXPECT_EQ(captured[0].content_hash, "doc-abc");  // self-dedup key (debounce)
}

TEST_F(SPCPipelineTest, OnDocumentWrittenIsNoOpWhenSeamUnset) {
    // No SetDocSummaryEnqueue → feature off → the hook is a safe no-op.
    pipeline_->OnDocumentWritten("doc-xyz", "ns-1");
    SUCCEED();
}

// ============================================================
// Branch coverage: memory-session edge paths, ProcessParsed (F42) direct entry,
// partial parse status, malformed custom metadata.
// ============================================================

// Memory-session task with EMPTY content_hash → the text.empty() branch marks the
// doc ready with zero blocks (no embed/assemble), distinct from the normal
// memory write path.
TEST_F(SPCPipelineTest, MemorySession_EmptyText_DoneZeroBlocks) {
    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask("/n/a", "text/plain", doc_id);
    task_uptr->source_type = "memory_session";
    task_uptr->content_hash = "";  // no Q+A text

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, 0) << task_uptr->error_message;
    EXPECT_EQ(task_uptr->stage, SPCStage::kDone);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);
    EXPECT_EQ(StatusOf(doc_id), DocStatus::kReady);
}

// Memory-session task pre-cancelled → the first cancel check in the memory path
// returns -1 / kCancelled before any embedding.
TEST_F(SPCPipelineTest, MemorySession_PreCancelled) {
    auto task_uptr = MakeTask("/n/a", "text/plain", "01JTESTDOC0000000000000099");
    task_uptr->source_type = "memory_session";
    task_uptr->content_hash = "q\n---\na";
    task_uptr->cancelled.store(true);

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task_uptr->stage, SPCStage::kCancelled);
    EXPECT_EQ(TotalBlocks(), 0);
}

// ProcessParsed is the F42 async entry point (parse already done by F06). Driving
// it directly with a hand-built multi-paragraph ParsedDoc exercises the same
// post-parse stages (chunk → F08 → embed → assemble → write) without the parser
// subprocess — the dedicated Plan-B seam.
TEST_F(SPCPipelineTest, ProcessParsed_DirectEntry_WritesBlocks) {
    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    task_uptr->processing_level = 3;

    cortrix::spc::ParsedDoc d;
    d.status = cortrix::spc::ParserErrorCode::kOk;
    d.parser_name = "docling";
    d.metadata.filename = "direct.txt";
    d.metadata.page_count = 1;
    d.metadata.doc_language = "en";
    cortrix::spc::ParsedPage page;
    page.page_num = 1;
    page.page_text = "body";
    page.page_metadata.page_num = 1;
    page.page_metadata.page_confidence = 0.95f;
    // Two large paragraphs so the chunker yields parents + children.
    for (const char* body : {"ALPHA", "BETA"}) {
        cortrix::spc::ParsedChunk c;
        c.text = std::string(body) + " ";
        for (int i = 0; i < 80; ++i) c.text += std::string(body) + " ";
        c.page = 1;
        c.type = cortrix::spc::ChunkType::TEXT;
        c.confidence = 0.95f;
        c.language = "en";
        page.paragraphs.push_back(c);
    }
    d.pages.push_back(page);

    int rc = pipeline_->ProcessParsed(d, *task_uptr, *facade_);
    ASSERT_EQ(rc, 0) << task_uptr->error_message;
    EXPECT_EQ(task_uptr->stage, SPCStage::kDone);
    EXPECT_GT(BlocksOf(doc_id).size(), 0u);
}

// ProcessParsed pre-cancelled → the cancel check before chunking returns
// -1 / kCancelled (the F42 path's own cancel guard).
TEST_F(SPCPipelineTest, ProcessParsed_PreCancelled) {
    auto task_uptr = MakeTask(txt_path_, "text/plain", "01JTESTDOC0000000000000077");
    task_uptr->cancelled.store(true);
    cortrix::spc::ParsedDoc d;
    d.status = cortrix::spc::ParserErrorCode::kOk;
    cortrix::spc::ParsedPage page;
    page.page_num = 1;
    cortrix::spc::ParsedChunk c;
    c.text = "x";
    c.page = 1;
    page.paragraphs.push_back(c);
    d.pages.push_back(page);

    int rc = pipeline_->ProcessParsed(d, *task_uptr, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task_uptr->stage, SPCStage::kCancelled);
}

// A parse result carrying failed_pages → the F08 generator records
// parse_status="partial" (the `d.failed_pages.empty() ? "ok" : "partial"` false
// branch). Verified end-to-end: the doc still writes successfully.
TEST_F(SPCPipelineTest, PartialParse_StatusReflectedInMeta) {
    const std::string para(4400, 'p');  // > parent_size → own parent
    const std::string json =
        std::string(R"JSON({"status":0,"parser":"docling",)JSON") +
        R"JSON("metadata":{"filename":"partial.txt","page_count":2,"doc_language":"en"},)JSON" +
        R"JSON("pages":[{"page_num":1,"page_text":"p","page_metadata":{"page_num":1,)JSON" +
        R"JSON("parser_used":"docling","page_confidence":0.95,"is_scan_page":false,"char_count":1},)JSON" +
        R"JSON("paragraphs":[{"text":")JSON" + para +
        R"JSON(","page":1,"type":"TEXT","confidence":0.95,"language":"en"}]}],)JSON" +
        R"JSON("failed_pages":[2]})JSON";
    RebuildFactory(json);

    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    int rc = pipeline_->Process(*task_uptr, *facade_);
    ASSERT_EQ(rc, 0) << task_uptr->error_message;

    // The F08 META block carries the parse stats; the doc wrote successfully with
    // a non-empty failed_pages set driving the "partial" status branch.
    int meta_count = 0;
    for (const auto& b : BlocksOf(doc_id)) {
        if (b.block_type == static_cast<int>(kBlockMeta)) ++meta_count;
    }
    EXPECT_EQ(meta_count, 1);
}

// task.metadata_json present but MALFORMED → the custom-metadata parse is
// discarded (the `!cm.is_discarded() && cm.is_object()` false branch), and the
// pipeline still completes (custom metadata is simply not folded in).
TEST_F(SPCPipelineTest, MalformedCustomMetadata_IgnoredNotFatal) {
    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    task_uptr->metadata_json = "{ this is : not valid json";  // malformed

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, 0) << task_uptr->error_message;
    EXPECT_EQ(task_uptr->stage, SPCStage::kDone);
    EXPECT_GT(BlocksOf(doc_id).size(), 0u);
}

// ============================================================
// Store-write failure branches (SetFailNextOps seam, F23 section 4.5). Each path's first
// store CRUD op after BeginWrite is forced to return -1; the pipeline must roll
// the txn back, set kError with the path-specific message, and persist nothing.
// The fault is armed AFTER CreateDoc() (which itself calls doc_create) so the
// injected count lands on the intended pipeline op, not the fixture's setup.
// ============================================================

// Memory path: the lone block_insert is the first store op inside the txn, so
// SetFailNextOps(1) fails it -> Rollback + "block_insert failed for memory block".
TEST_F(SPCPipelineTest, MemorySession_BlockInsertFailure_RollsBack) {
    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask("/n/a", "text/plain", doc_id);
    task_uptr->source_type = "memory_session";
    task_uptr->content_hash = "q\n---\na";

    ASSERT_NE(ConcreteStore(), nullptr);
    ConcreteStore()->SetFailNextOps(1);  // fail the memory block_insert

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task_uptr->stage, SPCStage::kError);
    EXPECT_NE(task_uptr->error_message.find("block_insert"), std::string::npos);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);  // rolled back
}

// L1 path: the metadata-only block_insert is the first store op in the L1 txn, so
// SetFailNextOps(1) fails it -> Rollback + "block_insert failed for L1 metadata block".
TEST_F(SPCPipelineTest, L1_BlockInsertFailure_RollsBack) {
    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    task_uptr->processing_level = 1;

    ASSERT_NE(ConcreteStore(), nullptr);
    ConcreteStore()->SetFailNextOps(1);  // fail the L1 block_insert

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task_uptr->stage, SPCStage::kError);
    EXPECT_NE(task_uptr->error_message.find("block_insert"), std::string::npos);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);
}

// Stage 5 (normal L2 path so there is no vector AddPoints before the parents):
// parents are written before child blocks, so the FIRST store op in the write
// phase is parent_insert; SetFailNextOps(1) fails it -> Rollback +
// "parent_insert failed". L2 avoids the embedding/AddPoints step entirely, which
// keeps parent_insert as op #1 after BeginWrite.
TEST_F(SPCPipelineTest, Stage5_ParentInsertFailure_RollsBack) {
    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    task_uptr->processing_level = 2;  // chunks + parents, no embedding

    ASSERT_NE(ConcreteStore(), nullptr);
    ConcreteStore()->SetFailNextOps(1);  // fail the first parent_insert

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task_uptr->stage, SPCStage::kError);
    EXPECT_NE(task_uptr->error_message.find("parent_insert"), std::string::npos);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);  // rolled back, nothing persisted
}

// Stage 5 child block_insert failure: a single-parent doc means the write order
// is parent_insert (op #1) then the child block_insert (op #2). SetFailNextOps(2)
// fails both, but the pipeline aborts at the parent_insert (op #1) first, so this
// instead pins that an injected fault count > the parents still rolls back with no
// partial state. (The child-only block_insert arm with zero parents is the
// flat-fallback path, not forced here - recorded in the report.)
TEST_F(SPCPipelineTest, Stage5_MultiFaultStillRollsBack) {
    // One paragraph larger than parent_size -> exactly one parent, a few children.
    const std::string para(4400, 'z');
    const std::string json =
        std::string(R"JSON({"status":0,"parser":"docling",)JSON") +
        R"JSON("metadata":{"filename":"one.txt","page_count":1,"doc_language":"en"},)JSON" +
        R"JSON("pages":[{"page_num":1,"page_text":"z","page_metadata":{"page_num":1,)JSON" +
        R"JSON("parser_used":"docling","page_confidence":0.95,"is_scan_page":false,"char_count":1},)JSON" +
        R"JSON("paragraphs":[{"text":")JSON" + para +
        R"JSON(","page":1,"type":"TEXT","confidence":0.95,"language":"en"}]}]})JSON";
    RebuildFactory(json);

    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    task_uptr->processing_level = 2;

    ASSERT_NE(ConcreteStore(), nullptr);
    ConcreteStore()->SetFailNextOps(2);

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task_uptr->stage, SPCStage::kError);
    // The pipeline aborts on the FIRST injected fault, so one armed fault is
    // left over -- drain it so the read-back below hits the real store.
    ConcreteStore()->SetFailNextOps(0);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);
}

// ============================================================
// Round-3 residue: memory-path AddPoints failure (vec index, not the store seam)
// and the valid-but-non-object custom-metadata arm.
// ============================================================

// Memory path: force the vector AddPoints to fail (FakeIndex knob, not the store
// seam, because the failing op here is on facade.vec_index()). This reaches the
// memory-path AddPoints rollback branch (set_add_should_fail) -> kError + "AddPoints"
// + Rollback, which the store-seam tests cannot hit (they fail block_insert).
TEST_F(SPCPipelineTest, MemorySession_AddPointsFailure_RollsBack) {
    fake_index_->set_add_should_fail(true);
    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask("/n/a", "text/plain", doc_id);
    task_uptr->source_type = "memory_session";
    task_uptr->content_hash = "q\n---\na";  // non-empty -> L3 embed -> vec_points set

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task_uptr->stage, SPCStage::kError);
    EXPECT_NE(task_uptr->error_message.find("AddPoints"), std::string::npos);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);  // rolled back before block_insert
}

// task.metadata_json is VALID JSON but NOT an object (a JSON array) -> the
// `!cm.is_discarded() && cm.is_object()` guard takes is_discarded()==false but
// is_object()==false, so custom_metadata is left unset. Distinct from the
// malformed (discarded) case and the valid-object happy path. Pipeline completes.
TEST_F(SPCPipelineTest, NonObjectCustomMetadata_NotFoldedButCompletes) {
    std::string doc_id = CreateDoc();
    auto task_uptr = MakeTask(txt_path_, "text/plain", doc_id);
    task_uptr->metadata_json = "[1, 2, 3]";  // valid JSON, not an object

    int rc = pipeline_->Process(*task_uptr, *facade_);
    EXPECT_EQ(rc, 0) << task_uptr->error_message;
    EXPECT_EQ(task_uptr->stage, SPCStage::kDone);
    EXPECT_GT(BlocksOf(doc_id).size(), 0u);
}

}  // namespace
}  // namespace cortrix
