// R7 branch-coverage supplement for src/spc/spc_pipeline.cpp.
//
// The existing test_spc_pipeline.cpp drives the core orchestration (L0-L3, memory
// path, store/WAL fault injection, the NullEnricher path) but never installs the
// optional D3.5 seams, so their branches in Process()/ProcessParsed() are dead in
// the coverage build:
//   - the EnricherChain path (use_chain): I1 F03→F35→F38 fail-soft serial, the
//     contextualized_embedding flag (kFlagExtHasContextualized), WriteEnrichment +
//     WriteContextualized, and the F38 hype-question Block assembly + embed loop;
//   - the SparseIndexRegistry path (Q4 F40): EmbedBatchWithSparse + per-child
//     sparse_vec persist + inverted-index Add;
//   - the cleaning-config resolver field branches (anomaly off, threshold).
//
// This file installs real seams (a real EnricherChain with fake F03/F35 + a real
// HyPEEnricher driven by MockLlmClient; a real in-memory SparseIndexRegistry) over
// the SAME pool/façade plumbing test_spc_pipeline.cpp uses, and asserts the
// observable effects (sparse_vec rows, hype_question blocks, contextualized columns,
// dedup survivors). It is a standalone NEW file (does not touch the existing one).
//
// It also carries the corrected F10 dedup-disabled regression: the original
// SPCPipelineTest.F10_NsConfigResolver_DisablesDedup asserted that a 4400-'x'
// paragraph survives dedup-disabled as a child whose content_text == the full 4400
// string. That is impossible: para(4400,'x') has no separators, so the F34 child
// splitter (RecursiveChunker child_size=200) hard-cuts it into ~8 ~600-char slices
// per parent — no child ever equals the 4400-char paragraph, so the count is always
// 0 (release AND coverage). The intent (dedup-off keeps duplicates) is preserved
// here by counting TOTAL survivors vs DISTINCT texts instead.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/spc/parser_factory.h"
#include "cortrix/spc/docling_parser.h"
#include "cortrix/spc/paddleocr_parser.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc_enricher.h"            // NullEnricher (ctor injection)
#include "cortrix/spc/enricher_chain.h"      // I1 EnricherChain
#include "cortrix/spc/hype_enricher.h"       // F38 HyPEEnricher (real, drives the hype side channel)
#include "cortrix/spc/spc_task.h"
#include "cortrix/chunker/parent_child_chunker.h"
#include "cortrix/retrieval/sparse_index_registry.h"  // Q4 F40

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
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/write_coordinator.h"
#include "cortrix/id/hash.h"
#include "cortrix/id/ulid.h"

#include "cortrix/async/task_info.h"
#include "cortrix/common/block_types.h"
#include "cortrix/common/block_header.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include "mock_llm_client.h"

namespace cortrix {
namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

// ============================================================
// Test doubles — same shapes as test_spc_pipeline.cpp's pool plumbing.
// ============================================================

class FakeIndex : public cortrix::store::IIndex, public cortrix::store::IVectorStore {
public:
    explicit FakeIndex(std::size_t footprint = 0) : footprint_(footprint) {}
    Status AddPoint(const float*, uint64_t id, const observability::TraceContext*) override {
        if (add_should_fail_) return Status(StatusCode::kUnavailable, "forced add failure");
        added_ids_.push_back(id);
        return Status::Ok();
    }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override { return Status::Ok(); }
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

// Fake F03-style enricher (entities + summary). Mirrors test_enricher_chain.cpp.
class FakeSummaryEnricher : public cortrix::spc::ISpcEnricher {
public:
    cortrix::spc::EnrichResult Enrich(const std::string&,
                                      const cortrix::spc::DocumentMetadata&,
                                      const cortrix::spc::ChunkContext&) override {
        cortrix::spc::EnrichResult r;
        r.summary = "r7 fake summary";
        r.enricher_name = "LlmEnricher";  // maps to F07 enricher token "llm"
        r.enriched_score = 0.5f;
        cortrix::spc::Entity e;
        e.text = "Cortrix";
        e.type = "PRODUCT";
        r.entities.push_back(e);
        return r;
    }
    std::vector<cortrix::spc::EnrichResult> EnrichBatch(
        const std::vector<cortrix::spc::ChunkContext>& c) override {
        std::vector<cortrix::spc::EnrichResult> out;
        for (const auto& ctx : c)
            out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "LlmEnricher"; }
};

// Fake F35-style enricher: stamps a contextualized embedding (drives the
// kFlagExtHasContextualized bit + WriteContextualized branch in the pipeline).
class FakeContextualEnricher : public cortrix::spc::ISpcEnricher {
public:
    cortrix::spc::EnrichResult Enrich(const std::string& text,
                                      const cortrix::spc::DocumentMetadata&,
                                      const cortrix::spc::ChunkContext&) override {
        cortrix::spc::EnrichResult r;
        r.contextualized_text = "ctx: " + text;
        r.contextualized_embedding = std::vector<float>(128, 0.01f);
        r.contextualized_status = 1;  // generated
        return r;
    }
    std::vector<cortrix::spc::EnrichResult> EnrichBatch(
        const std::vector<cortrix::spc::ChunkContext>& c) override {
        std::vector<cortrix::spc::EnrichResult> out;
        for (const auto& ctx : c)
            out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "ContextualRetrievalEnricher"; }
};

const char* kDoclingMultiPara = R"JSON({
  "status": 0, "parser": "docling",
  "metadata": {"filename": "doc.txt", "page_count": 1, "doc_language": "en"},
  "pages": [{"page_num": 1, "page_text": "para one para two",
             "page_metadata": {"page_num": 1, "parser_used": "docling",
                               "page_confidence": 0.95, "is_scan_page": false, "char_count": 28},
             "paragraphs": [
               {"text": "ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA ALPHA",
                "page": 1, "type": "TEXT", "confidence": 0.95, "language": "en"},
               {"text": "BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA BETA",
                "page": 1, "type": "TEXT", "confidence": 0.95, "language": "en"}
             ]}]})JSON";

// ============================================================
// Fixture
// ============================================================

class SPCPipelineR7Test : public ::testing::Test {
protected:
    void SetUp() override {
        id::SetDeploymentHashKeyForTesting({0x0123456789abcdefULL, 0xfedcba9876543210ULL});
        tmp_root_ = std::filesystem::temp_directory_path() /
                    ("spc_pipe_r7_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(tmp_root_);
        config_.data_root = tmp_root_.string();
        std::filesystem::create_directories(tmp_root_ / "test-ns-u");

        pool_ = MakePool();
        ASSERT_TRUE(pool_->AdmitCreate("test-ns", 100).ok());
        ASSERT_NE(fake_index_, nullptr);
        facade_ = std::make_unique<resource::NamespaceFacade>(*pool_, "test-ns");
        ASSERT_TRUE(facade_->Acquire().ok());

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        assembler_ = std::make_unique<BlockAssembler>();
        chunker_ = std::make_unique<cortrix::chunker::ParentChildChunker>(
            cortrix::chunker::ChunkerConfig{});
        RebuildFactory(kDoclingMultiPara);

        txt_path_ = WriteTmp(".txt", "real text on disk\n");
    }

    void TearDown() override {
        for (const auto& p : tmp_files_) std::remove(p.c_str());
        facade_.reset();
        pool_.reset();
        std::error_code ec;
        std::filesystem::remove_all(tmp_root_, ec);
    }

    std::string WriteTmp(const std::string& suffix, const std::string& content) {
        char tmpl[] = "/tmp/cortrix_spcr7_XXXXXX";
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
        pc.script_path = script;
        factory_->SetFallbackParser(std::make_unique<cortrix::spc::PaddleOCRParser>(pc));
        pipeline_ = std::make_unique<SPCPipeline>(
            *factory_, *chunker_, *embedder_, *assembler_, enricher_);
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
            .WillByDefault(Invoke([this](const std::string&) {
                auto idx = std::make_unique<FakeIndex>(0);
                fake_index_ = idx.get();
                return Result<std::unique_ptr<cortrix::store::IIndex>>(std::move(idx));
            }));
        return std::make_unique<resource::DefaultNamespacePool>(
            &index_factory_, MakeRealCoordFactory(), &ns_router_, &unit_router_, config_);
    }

    std::string CreateDoc(const std::string& source_path = "/spc/doc.txt") {
        cortrix::CortrixDoc doc;
        doc.source_type = "test";
        doc.source_path = source_path;
        EXPECT_EQ(facade_->store().doc_create(doc), 0);
        EXPECT_FALSE(doc.doc_id.empty());
        return doc.doc_id;
    }

    std::vector<cortrix::CortrixBlock> BlocksOf(const std::string& doc_id) {
        std::vector<cortrix::CortrixBlock> out;
        EXPECT_EQ(facade_->store().block_get_by_doc(doc_id, out), 0);
        return out;
    }

    std::unique_ptr<SPCTask> MakeTask(const std::string& source_path,
                                       const std::string& mime_type,
                                       const std::string& doc_id) {
        auto t = std::make_unique<SPCTask>();
        t->task_id = 1;
        t->doc_id = doc_id;
        t->namespace_name = "test-ns";
        t->source_path = source_path;
        t->mime_type = mime_type;
        t->priority = SPCPriority::kP0;
        t->stage = SPCStage::kQueued;
        t->cancelled.store(false);
        return t;
    }

    int CountSql(const char* sql) {
        sqlite3* db = facade_->store().db_handle();
        EXPECT_NE(db, nullptr);
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, nullptr), SQLITE_OK);
        int n = 0;
        if (st && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return n;
    }

    // ── pool plumbing ──
    std::filesystem::path tmp_root_;
    resource::F05Config config_;
    NiceMock<MockIndexFactory> index_factory_;
    NiceMock<MockNSRouter> ns_router_;
    NiceMock<MockUnitRouter> unit_router_;
    FakeIndex* fake_index_ = nullptr;
    std::unique_ptr<resource::DefaultNamespacePool> pool_;
    std::unique_ptr<resource::NamespaceFacade> facade_;

    // ── real pipeline ──
    std::unique_ptr<cortrix::spc::DocumentParserFactory> factory_;
    std::unique_ptr<cortrix::chunker::ParentChildChunker> chunker_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<BlockAssembler> assembler_;
    cortrix::spc::NullEnricher enricher_;
    std::unique_ptr<SPCPipeline> pipeline_;

    std::vector<std::string> tmp_files_;
    std::string txt_path_;
};

// Build a MockLlmClient that returns K newline-separated questions for any prompt,
// so HyPEEnricher::GenerateHypeQuestions parses exactly K=3 questions (success path).
std::shared_ptr<llm::MockLlmClient> MakeHypeLlm() {
    auto mock = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse ok;
    ok.status = Status::Ok();
    ok.finish_reason = "stop";
    ok.model = "gpt-4o-mini";
    ok.content = "What is alpha?\nWhy beta?\nHow gamma?";  // exactly 3 lines (K default)
    EXPECT_CALL(*mock, Chat(_, _)).WillRepeatedly(Return(ok));
    return mock;
}

// ============================================================
// F10 corrected dedup-disabled regression (replaces the broken
// SPCPipelineTest.F10_NsConfigResolver_DisablesDedup assertion).
// ============================================================

// Two byte-identical large paragraphs, dedup DISABLED via the NS resolver. Each
// paragraph (4400 'x', no separators) is split by the F34 child splitter into
// several ~600-char children; the two paragraphs yield IDENTICAL child sequences.
// With dedup off, every child survives, so the surviving child count strictly
// exceeds the number of DISTINCT child texts (duplicates were kept). The mirror
// (dedup on) collapses to exactly the distinct count. This proves the resolved NS
// config (dedup_enabled=false) took effect — without the impossible
// "content_text == full 4400-char paragraph" assumption of the original test.
TEST_F(SPCPipelineR7Test, F10_NsResolverDisablesDedup_KeepsDuplicateChildren) {
    const std::string para(4400, 'x');
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

    pipeline_->SetCleaningConfigResolver([](const std::string&) {
        cortrix::spc::CleaningConfig c;
        c.dedup_enabled = false;  // NS override under test
        return c;
    });

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;

    std::vector<std::string> child_texts;
    for (const auto& b : BlocksOf(doc_id)) {
        if (b.block_type == static_cast<int>(kBlockFile) && !b.child_id.empty())
            child_texts.push_back(b.content_text);
    }
    std::unordered_set<std::string> distinct(child_texts.begin(), child_texts.end());
    ASSERT_GT(child_texts.size(), 0u);
    EXPECT_GT(child_texts.size(), distinct.size())
        << "dedup disabled via NS config → duplicate children must survive "
           "(total=" << child_texts.size() << " distinct=" << distinct.size() << ")";
}

// Mirror: with the resolver left default (dedup ENABLED) the identical children
// collapse — surviving child texts are all distinct. Confirms the resolver's false
// in the test above is what kept the duplicates (not some other effect).
TEST_F(SPCPipelineR7Test, F10_DefaultResolverEnablesDedup_CollapsesDuplicates) {
    const std::string para(4400, 'x');
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
    // No resolver installed → DataCleaner keeps its default config (dedup on).

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;

    std::vector<std::string> child_texts;
    for (const auto& b : BlocksOf(doc_id)) {
        if (b.block_type == static_cast<int>(kBlockFile) && !b.child_id.empty())
            child_texts.push_back(b.content_text);
    }
    std::unordered_set<std::string> distinct(child_texts.begin(), child_texts.end());
    ASSERT_GT(child_texts.size(), 0u);
    EXPECT_EQ(child_texts.size(), distinct.size())
        << "default config dedup on → no two surviving children share text";
}

// Resolver that turns anomaly detection OFF (the other §3.4 boolean field). Exercises
// the resolver SetConfig branch with anomaly_detection_enabled=false (DetectAnomaly
// early-returns). The doc still writes; no child carries a cleaning.* anomaly key.
TEST_F(SPCPipelineR7Test, F10_NsResolverDisablesAnomaly_NoAnomalyKeys) {
    pipeline_->SetCleaningConfigResolver([](const std::string&) {
        cortrix::spc::CleaningConfig c;
        c.anomaly_detection_enabled = false;
        return c;
    });

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;

    int with_anomaly = 0;
    for (const auto& b : BlocksOf(doc_id)) {
        if (b.metadata_json.find("cleaning.anomaly_reason") != std::string::npos) ++with_anomaly;
    }
    EXPECT_EQ(with_anomaly, 0) << "anomaly detection disabled → no cleaning.anomaly_reason keys";
    EXPECT_GT(BlocksOf(doc_id).size(), 0u);
}

// ============================================================
// EnricherChain (use_chain) path — F03 + F35 + F38 installed.
// ============================================================

// A full F03→F35→F38 chain over the L3 path. Exercises:
//   - the use_chain branch (enricher_chain_ && AnyAvailable());
//   - EnrichChunks + the parent_text_by_id / src_*_ids provenance build;
//   - the contextualized_embedding flag (kFlagExtHasContextualized) + WriteContextualized;
//   - WriteEnrichment (enriched_score + entities);
//   - the F38 hype-question Block assembly + embed loop (block_type=16 rows persisted).
TEST_F(SPCPipelineR7Test, EnricherChain_F03F35F38_PersistsEnrichmentAndHypeBlocks) {
    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>());     // F03
    chain.Append(std::make_shared<FakeContextualEnricher>());  // F35
    chain.Append(std::make_shared<cortrix::spc::HyPEEnricher>(
        cortrix::spc::HyPEConfig{}, MakeHypeLlm(), /*parent_store=*/nullptr));  // F38
    ASSERT_TRUE(chain.AnyAvailable());
    pipeline_->SetEnricherChain(&chain);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;
    EXPECT_EQ(task->stage, SPCStage::kDone);

    // F03: enriched_score + entities persisted (WriteEnrichment branch).
    EXPECT_GT(CountSql("SELECT COUNT(*) FROM blocks WHERE enriched_score > 0"), 0);
    EXPECT_GT(CountSql("SELECT COUNT(*) FROM entities"), 0);

    // F38: hype_question blocks (block_type=16 = kBlockHypeQuestion) written, one
    // set per source child (K=3 each). Their presence proves the hype assembly +
    // embed loop ran (level>=3 && !hype_by_child.empty()).
    EXPECT_GT(CountSql("SELECT COUNT(*) FROM blocks WHERE block_type = 16"), 0);

    // Each F08 META + child + hype block landed; child blocks present.
    EXPECT_GT(CountSql("SELECT COUNT(*) FROM blocks WHERE child_id IS NOT NULL AND child_id != ''"), 0);

    // The chain reset to nullptr is a valid no-op afterward.
    pipeline_->SetEnricherChain(nullptr);
}

// Chain installed but NO member available (all IsAvailable()==false) → the use_chain
// gate (AnyAvailable()) is false AND the ctor NullEnricher is also unavailable, so the
// whole enrich stage short-circuits (no entities, no hype). Confirms the false arm of
// the `(use_chain || enricher_.IsAvailable())` guard with a non-null but empty-effect
// chain (distinct from the nullptr-chain reset already covered elsewhere).
TEST_F(SPCPipelineR7Test, EnricherChain_NoneAvailable_ShortCircuits) {
    // HyPEEnricher with a NULL llm client => IsAvailable()==false.
    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<cortrix::spc::HyPEEnricher>(
        cortrix::spc::HyPEConfig{}, /*llm_client=*/nullptr, /*parent_store=*/nullptr));
    EXPECT_FALSE(chain.AnyAvailable());
    pipeline_->SetEnricherChain(&chain);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;

    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM entities"), 0);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM blocks WHERE block_type = 16"), 0);
    EXPECT_GT(BlocksOf(doc_id).size(), 0u);  // chunks + META still written
}

// Chain on the L2 path (no embedding): the enrich stage still runs (it precedes
// embed and only needs text), so F03 entities persist, but the F38 hype blocks are
// skipped (the hype assembly is gated on level>=3). Covers the "chain ran but hype
// blocks skipped" arm — the `level >= 3 && !hype_by_child.empty()` false branch.
TEST_F(SPCPipelineR7Test, EnricherChain_L2_EnrichesButSkipsHypeBlocks) {
    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>());
    chain.Append(std::make_shared<cortrix::spc::HyPEEnricher>(
        cortrix::spc::HyPEConfig{}, MakeHypeLlm(), nullptr));
    pipeline_->SetEnricherChain(&chain);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 2;  // chunks, no embedding → no hype blocks
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;

    EXPECT_GT(CountSql("SELECT COUNT(*) FROM entities"), 0);     // F03 ran on L2
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM blocks WHERE block_type = 16"), 0);  // no hype on L2
}

// ============================================================
// enrich_state coverage rows (addendum §3.7) — the per-chunk SoT the retry
// sweeper scans. One row per enriched-eligible child; 'ok' when every owed
// chain member succeeded, 'pending_retry' + failed_members csv otherwise.
// ============================================================

// F03-slot fake that always degrades (step status != 0, the F03/F38 failure shape).
class FailingSummaryEnricher : public cortrix::spc::ISpcEnricher {
public:
    cortrix::spc::EnrichResult Enrich(const std::string&,
                                      const cortrix::spc::DocumentMetadata&,
                                      const cortrix::spc::ChunkContext&) override {
        cortrix::spc::EnrichResult r;
        r.status = 3;  // any non-zero step status (kLlmApi-shaped degrade)
        r.error_msg = "CX_ERR_ENRICHER_LLM_API: synthetic transport burst";
        return r;
    }
    std::vector<cortrix::spc::EnrichResult> EnrichBatch(
        const std::vector<cortrix::spc::ChunkContext>& c) override {
        std::vector<cortrix::spc::EnrichResult> out;
        for (const auto& ctx : c)
            out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "LlmEnricher"; }
};

// F35-style fake failing the F35 way: the step status stays 0 (fail-soft) and the
// outcome is only visible in contextualized_status == 2 — exactly the shape the
// member-aware debt detection must catch.
class FailingContextualEnricher : public cortrix::spc::ISpcEnricher {
public:
    cortrix::spc::EnrichResult Enrich(const std::string&,
                                      const cortrix::spc::DocumentMetadata&,
                                      const cortrix::spc::ChunkContext&) override {
        cortrix::spc::EnrichResult r;
        r.contextualized_status = 2;  // failed (LLM error inside F35)
        r.error_msg = "CX_ERR_F35_LLM_FAILED: synthetic";
        return r;
    }
    std::vector<cortrix::spc::EnrichResult> EnrichBatch(
        const std::vector<cortrix::spc::ChunkContext>& c) override {
        std::vector<cortrix::spc::EnrichResult> out;
        for (const auto& ctx : c)
            out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "f35_contextual_retrieval"; }
};

// All chain members succeed → every child row lands status='ok', no retry stamp,
// and there is exactly one enrich_state row per child block.
TEST_F(SPCPipelineR7Test, EnrichState_OkChainWritesOkRowPerChild) {
    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>());
    chain.Append(std::make_shared<FakeContextualEnricher>());
    chain.Append(std::make_shared<cortrix::spc::HyPEEnricher>(
        cortrix::spc::HyPEConfig{}, MakeHypeLlm(), nullptr));
    pipeline_->SetEnricherChain(&chain);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    ASSERT_EQ(pipeline_->Process(*task, *facade_), 0) << task->error_message;

    const int children =
        CountSql("SELECT COUNT(*) FROM blocks WHERE child_id IS NOT NULL AND child_id != ''");
    ASSERT_GT(children, 0);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state"), children);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE status='ok'"), children);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE next_retry_at IS NOT NULL"), 0);
    pipeline_->SetEnricherChain(nullptr);
}

// F03 slot fails (step status != 0) → pending_retry rows owing exactly f03, with
// the retry stamp and the error detail.
TEST_F(SPCPipelineR7Test, EnrichState_F03FailureOwedAsPendingRetry) {
    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<FailingSummaryEnricher>());
    pipeline_->SetEnricherChain(&chain);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    ASSERT_EQ(pipeline_->Process(*task, *facade_), 0) << task->error_message;

    const int rows = CountSql("SELECT COUNT(*) FROM enrich_state");
    ASSERT_GT(rows, 0);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE status='pending_retry'"), rows);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE failed_members='f03'"), rows);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE next_retry_at IS NOT NULL"), rows);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE last_error LIKE 'CX_ERR%'"), rows);
    pipeline_->SetEnricherChain(nullptr);
}

// F35 fail-soft (step status 0, contextualized_status 2) → the member-aware
// detection still owes f35; the ok F03 head is NOT owed.
TEST_F(SPCPipelineR7Test, EnrichState_F35SoftFailureDetectedViaContextualizedStatus) {
    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>());        // ok F03
    chain.Append(std::make_shared<FailingContextualEnricher>());  // f35 soft-fail
    pipeline_->SetEnricherChain(&chain);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    ASSERT_EQ(pipeline_->Process(*task, *facade_), 0) << task->error_message;

    const int rows = CountSql("SELECT COUNT(*) FROM enrich_state");
    ASSERT_GT(rows, 0);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE failed_members='f35'"), rows);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE status='pending_retry'"), rows);
    pipeline_->SetEnricherChain(nullptr);
}

// F38 hype degrade (LLM fails inside GenerateHypeQuestions → step status != 0)
// → rows owe exactly f38 while the ok F03 head stays un-owed.
TEST_F(SPCPipelineR7Test, EnrichState_HypeFailureOwedAsF38) {
    auto failing_llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("LLM_TRANSPORT: synthetic");
    EXPECT_CALL(*failing_llm, Chat(_, _)).WillRepeatedly(Return(fail));

    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>());
    chain.Append(std::make_shared<cortrix::spc::HyPEEnricher>(
        cortrix::spc::HyPEConfig{}, failing_llm, nullptr));
    pipeline_->SetEnricherChain(&chain);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    ASSERT_EQ(pipeline_->Process(*task, *facade_), 0) << task->error_message;

    const int rows = CountSql("SELECT COUNT(*) FROM enrich_state");
    ASSERT_GT(rows, 0);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE failed_members='f38'"), rows);
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state WHERE status='pending_retry'"), rows);
    pipeline_->SetEnricherChain(nullptr);
}

// No available enricher at all → the enrich stage never ran → no coverage debt
// concept → zero enrich_state rows (rows would otherwise fake 'ok' coverage).
TEST_F(SPCPipelineR7Test, EnrichState_NoEnricherWritesNoRows) {
    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    ASSERT_EQ(pipeline_->Process(*task, *facade_), 0) << task->error_message;
    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM enrich_state"), 0);
}

// ============================================================
// SparseIndexRegistry (Q4 F40) path.
// ============================================================

// With the sparse registry wired, the L3 embed switches to EmbedBatchWithSparse and
// the write phase persists each child's sparse_vec + indexes it. Exercises:
//   - the sparse_registry_ != nullptr branch in Stage 4 (EmbedBatchWithSparse);
//   - the per-child WriteSparseVec + GetOrOpen + sparse->Add branch in the write loop.
// Assert via blocks.sparse_vec being non-null for child rows.
TEST_F(SPCPipelineR7Test, SparseRegistry_PersistsSparseVecForChildren) {
    cortrix::retrieval::SparseIndexRegistry registry(/*data_dir=*/"");  // in-memory per-NS
    pipeline_->SetSparseIndexRegistry(&registry);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;
    EXPECT_EQ(task->stage, SPCStage::kDone);

    // sparse_vec persisted on at least one child row (the F40 write branch ran).
    EXPECT_GT(CountSql("SELECT COUNT(*) FROM blocks WHERE child_id IS NOT NULL "
                       "AND child_id != '' AND sparse_vec IS NOT NULL"), 0);

    pipeline_->SetSparseIndexRegistry(nullptr);  // reset is a valid no-op
}

// Sparse registry on the L2 path: L2 skips embedding entirely, so EmbedBatchWithSparse
// is NOT called and no sparse_vec is written — the sparse branch is gated behind
// level>=3. Covers the "registry set but level<3" arm (sparse path not taken).
TEST_F(SPCPipelineR7Test, SparseRegistry_L2_NoSparseVecWritten) {
    cortrix::retrieval::SparseIndexRegistry registry("");
    pipeline_->SetSparseIndexRegistry(&registry);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 2;
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;

    EXPECT_EQ(CountSql("SELECT COUNT(*) FROM blocks WHERE sparse_vec IS NOT NULL"), 0);
    EXPECT_GT(BlocksOf(doc_id).size(), 0u);
}

// Both seams together (chain + sparse) on L3 — the realistic production wiring; makes
// sure the interleaved write-phase branches (WriteEnrichment + WriteContextualized +
// WriteSparseVec + WriteScore for the SAME child row) all run without conflict.
TEST_F(SPCPipelineR7Test, ChainAndSparse_Combined_L3_AllPersisted) {
    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>());
    chain.Append(std::make_shared<FakeContextualEnricher>());
    pipeline_->SetEnricherChain(&chain);
    cortrix::retrieval::SparseIndexRegistry registry("");
    pipeline_->SetSparseIndexRegistry(&registry);

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    int rc = pipeline_->Process(*task, *facade_);
    ASSERT_EQ(rc, 0) << task->error_message;

    EXPECT_GT(CountSql("SELECT COUNT(*) FROM blocks WHERE enriched_score > 0"), 0);
    EXPECT_GT(CountSql("SELECT COUNT(*) FROM blocks WHERE child_id IS NOT NULL "
                       "AND child_id != '' AND sparse_vec IS NOT NULL"), 0);
    EXPECT_GT(CountSql("SELECT COUNT(*) FROM blocks WHERE semantic_score IS NOT NULL"), 0);
}

// ============================================================
// F38 hype embed-failure degrade (F38-8): when the embedder is asked to embed the
// hype question texts and fails, the hype blocks are dropped (non-fatal) but the doc
// still writes. We can't make the stub embedder fail directly, so instead we verify
// the symmetric guard: a chain whose F38 produces questions but the doc is L3 with a
// vector-add failure rolls everything back (the hype vec_points are part of the same
// AddPoints set). This exercises the AddPoints-failure rollback WITH hype blocks in
// the write set.
// ============================================================
TEST_F(SPCPipelineR7Test, EnricherChainHype_VecAddFailure_RollsBackAll) {
    cortrix::spc::EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>());
    chain.Append(std::make_shared<cortrix::spc::HyPEEnricher>(
        cortrix::spc::HyPEConfig{}, MakeHypeLlm(), nullptr));
    pipeline_->SetEnricherChain(&chain);

    fake_index_->set_add_should_fail(true);  // force AddPoints to fail

    std::string doc_id = CreateDoc();
    auto task = MakeTask(txt_path_, "text/plain", doc_id);
    task->processing_level = 3;
    int rc = pipeline_->Process(*task, *facade_);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(task->stage, SPCStage::kError);
    EXPECT_NE(task->error_message.find("AddPoints"), std::string::npos);
    EXPECT_EQ(BlocksOf(doc_id).size(), 0u);  // rolled back incl. hype + META blocks
}

}  // namespace
}  // namespace cortrix
