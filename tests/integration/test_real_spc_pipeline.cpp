/// @file test_real_spc_pipeline.cpp
/// @brief D3.5 Integration: Full SPC pipeline with real bge-m3 ONNX embeddings.
///
/// Tests end-to-end: Document → Parse → Chunk → Real Embed (bge-m3) →
///   Store (SQLite + HNSW 1024-dim) → Query (FTS + Vector + RRF) → Semantic results
///
/// SKIPPED if bge-m3 model files not present on disk.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

// Storage
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/cortrix_blob_local.h"
#include "cortrix/store/phnsw.h"
#include "cortrix/common/block_types.h"

// SPC pipeline
#include "cortrix/spc/recursive_chunker.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/block_assembler.h"

// Query engine
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/query_response.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/sql_executor.h"
#include "cortrix/config/config.h"

namespace cortrix {
namespace {

using namespace cortrix::store;  // PHnsw / PhnswConfig / IndexStats live in cortrix::store
namespace fs = std::filesystem;

// Find bge-m3 model directory (same logic as test_onnx_real_inference.cpp)
static std::string FindModelDir() {
    std::vector<std::string> candidates = {
        "models/bge-m3",
        "../models/bge-m3",
        "../../models/bge-m3",
        "../../../models/bge-m3",
    };
    const char* src_dir = std::getenv("CORTRIX_SOURCE_DIR");
    if (src_dir) {
        candidates.push_back(std::string(src_dir) + "/models/bge-m3");
    }
    for (const auto& dir : candidates) {
        if (fs::exists(dir + "/model.onnx") && fs::exists(dir + "/tokenizer.json")) {
            return dir;
        }
    }
    return "";
}

// Read a UTF-8 text file into a string. The test inputs are plain .txt files
// written by the fixtures below, so this is the in-process equivalent of the
// (removed in R6b) MVP TxtParser used purely to obtain text for the chunker.
// The parser path (DoclingParser / PaddleOCRParser) runs via a Python
// subprocess bridge and is not appropriate here — these tests exercise the
// chunk -> embed -> store -> query chain, not document parsing.
static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ==================================================================
// Real SPC Pipeline Integration Test Fixture
// ==================================================================

class RealSPCPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        model_dir_ = FindModelDir();
        if (model_dir_.empty()) {
            GTEST_SKIP() << "bge-m3 model not found, skipping real SPC pipeline test. "
                         << "Download model.onnx, model.onnx_data, and tokenizer.json "
                         << "to models/bge-m3/";
        }

        model_path_ = model_dir_ + "/model.onnx";
        tokenizer_path_ = model_dir_ + "/tokenizer.json";

        // Create temp directory for storage
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_real_spc_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);

        db_path_ = (test_dir_ / "cortrix.db").string();
        blob_dir_ = (test_dir_ / "blobs").string();
        vec_path_ = (test_dir_ / "vec").string();  // PHnsw uses a unit data dir (not a file)

        // Initialize storage with 1024-dim (real bge-m3 dimension)
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);

        blob_ = std::make_unique<CortrixBlobLocal>(blob_dir_);

        PhnswConfig cfg;
        cfg.dim = 1024;
        cfg.max_elements = 10000;
        vec_ = std::make_unique<PHnsw>(vec_path_, cfg);  // ctor runs Recover()

        // Initialize real ONNX embedder
        embedder_ = std::make_unique<OnnxEmbedder>(model_path_, 1024);
        embedder_->set_tokenizer_path(tokenizer_path_);
        Status s = embedder_->Init();
        ASSERT_TRUE(s.ok()) << s.message();
        ASSERT_TRUE(embedder_->is_real_model())
            << "Embedder should use real model, not stub";
    }

    void TearDown() override {
        embedder_.reset();
        if (store_) store_->Close();  // store_ is null when SetUp GTEST_SKIP'd before constructing it
        store_.reset();
        blob_.reset();
        vec_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    // Helper: ingest a document through SPC pipeline (read text → chunk → embed → store)
    int IngestDocument(const std::string& file_path, const std::string& mime_type,
                       std::string* out_doc_id = nullptr) {
        // Read text (plain .txt inputs; parser step replaced by ReadFile in R6b).
        // mime_type is still recorded on the doc record below.
        std::string text = ReadFile(file_path);
        if (text.empty()) return -1;

        // Chunk
        ChunkConfig chunk_cfg;
        chunk_cfg.chunk_size = 256;
        chunk_cfg.chunk_overlap = 30;
        RecursiveChunker chunker(chunk_cfg);
        auto chunks = chunker.Chunk(text);
        if (chunks.empty()) return -1;

        // Embed (real ONNX)
        std::vector<std::string> chunk_texts;
        for (const auto& c : chunks) chunk_texts.push_back(c.text);
        std::vector<EmbeddingResult> embeddings;
        Status s = embedder_->EmbedBatch(chunk_texts, &embeddings);
        if (!s.ok()) return -1;

        // Create doc record
        CortrixDoc doc;
        doc.source_type = "http_upload";
        doc.source_path = fs::path(file_path).filename().string();
        doc.content_hash = "sha256_real_test";
        doc.file_size = fs::file_size(file_path);
        doc.mime_type = mime_type;
        if (store_->doc_create(doc) != 0) return -1;

        // Store blocks + vectors
        BlockAssembler assembler;
        for (size_t i = 0; i < chunks.size(); ++i) {
            CortrixBlock block = assembler.Assemble(
                doc.doc_id, chunks[i], embeddings[i], kBlockFile);
            if (store_->block_insert(block) != 0) return -1;
            if (!vec_->AddPoint(embeddings[i].vector.data(), block.block_id).ok()) return -1;
        }

        store_->doc_update_status(doc.doc_id, DocStatus::kReady);
        if (out_doc_id) *out_doc_id = doc.doc_id;
        return static_cast<int>(chunks.size());
    }

    std::string model_dir_, model_path_, tokenizer_path_;
    fs::path test_dir_;
    std::string db_path_, blob_dir_, vec_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
    std::unique_ptr<CortrixBlobLocal> blob_;
    std::unique_ptr<PHnsw> vec_;
    std::unique_ptr<OnnxEmbedder> embedder_;
};

// ==================================================================
// Test 1: Full pipeline - ingest doc, then semantic vector search
// ==================================================================

TEST_F(RealSPCPipelineTest, IngestAndVectorSearch) {
    // Create a document about machine learning
    std::string doc_path = (test_dir_ / "ml_doc.txt").string();
    {
        std::ofstream f(doc_path);
        f << "Machine learning is a subset of artificial intelligence that "
          << "enables systems to learn from data. Deep learning uses neural "
          << "networks with many layers to model complex patterns. "
          << "Common algorithms include gradient descent, backpropagation, "
          << "and stochastic optimization. Training data is split into "
          << "training, validation, and test sets to evaluate model performance. "
          << "Overfitting occurs when a model memorizes training data instead "
          << "of learning generalizable patterns. Regularization techniques "
          << "like dropout and L2 penalty help prevent overfitting.\n";
    }

    std::string doc_id;
    int num_chunks = IngestDocument(doc_path, "text/plain", &doc_id);
    ASSERT_GT(num_chunks, 0) << "Should produce at least one chunk";
    ASSERT_FALSE(doc_id.empty());  // [D-I6] doc_id is a ULID string now

    std::cout << "[REAL SPC] Ingested " << num_chunks
              << " chunks with real 1024-dim embeddings" << std::endl;

    // Query with semantically related text
    EmbeddingResult query_emb;
    Status s = embedder_->Embed("How to prevent overfitting in neural networks?", &query_emb);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(query_emb.dim, 1024);

    auto results = vec_->Search(query_emb.vector.data(), /*top_k=*/3);
    ASSERT_GT(results.size(), 0u) << "Vector search should return results";

    // Verify result belongs to our document
    CortrixBlock blk;
    ASSERT_EQ(store_->block_get(results[0].first, blk), 0);
    EXPECT_EQ(blk.doc_id, doc_id);

    std::cout << "[REAL SPC] Vector search returned " << results.size()
              << " results, top distance=" << results[0].second << std::endl;
}

// ==================================================================
// Test 2: Semantic relevance - related query ranks higher than unrelated
// ==================================================================

TEST_F(RealSPCPipelineTest, SemanticRelevanceRanking) {
    // Ingest two documents on different topics
    std::string cooking_path = (test_dir_ / "cooking.txt").string();
    {
        std::ofstream f(cooking_path);
        f << "Italian cooking revolves around fresh ingredients and simple "
          << "techniques. Pasta is made from durum wheat semolina and water. "
          << "Traditional sauces include marinara, pesto, and carbonara. "
          << "Olive oil is the foundation of Mediterranean cuisine. "
          << "Fresh herbs like basil, oregano, and rosemary add flavor. "
          << "Pizza dough needs time to rise for proper texture.\n";
    }

    std::string physics_path = (test_dir_ / "physics.txt").string();
    {
        std::ofstream f(physics_path);
        f << "Quantum mechanics describes the behavior of particles at "
          << "the atomic and subatomic scale. The wave function encodes "
          << "probability amplitudes for measurement outcomes. "
          << "Heisenberg uncertainty principle limits simultaneous knowledge "
          << "of position and momentum. Entanglement allows correlations "
          << "between distant particles that defy classical explanation.\n";
    }

    std::string cooking_id, physics_id;
    ASSERT_GT(IngestDocument(cooking_path, "text/plain", &cooking_id), 0);
    ASSERT_GT(IngestDocument(physics_path, "text/plain", &physics_id), 0);

    // Query about food — should match cooking doc
    EmbeddingResult food_query;
    ASSERT_TRUE(embedder_->Embed("What ingredients are used in Italian cuisine?", &food_query).ok());

    auto results = vec_->Search(food_query.vector.data(), /*top_k=*/5);
    ASSERT_GT(results.size(), 0u);

    // Top result should be from cooking document
    CortrixBlock top_block;
    ASSERT_EQ(store_->block_get(results[0].first, top_block), 0);
    EXPECT_EQ(top_block.doc_id, cooking_id)
        << "Semantic search for 'Italian cuisine ingredients' should match cooking doc first";

    std::cout << "[SEMANTIC RANK] Food query → top result doc_id=" << top_block.doc_id
              << " (expected cooking_id=" << cooking_id << ")" << std::endl;

    // Query about science — should match physics doc
    EmbeddingResult sci_query;
    ASSERT_TRUE(embedder_->Embed("Explain quantum entanglement and wave functions", &sci_query).ok());

    results = vec_->Search(sci_query.vector.data(), /*top_k=*/5);
    ASSERT_GT(results.size(), 0u);

    ASSERT_EQ(store_->block_get(results[0].first, top_block), 0);
    EXPECT_EQ(top_block.doc_id, physics_id)
        << "Semantic search for 'quantum entanglement' should match physics doc first";

    std::cout << "[SEMANTIC RANK] Physics query → top result doc_id=" << top_block.doc_id
              << " (expected physics_id=" << physics_id << ")" << std::endl;
}

// ==================================================================
// Test 3: FTS + Vector combined (hybrid search via QueryPipeline)
// ==================================================================

TEST_F(RealSPCPipelineTest, HybridQueryPipeline) {
    std::string doc_path = (test_dir_ / "cortrix_doc.txt").string();
    {
        std::ofstream f(doc_path);
        f << "Cortrix is a semantic storage engine designed for AI agents. "
          << "It provides document ingestion through the SPC pipeline, which "
          << "splits, parses, and chunks documents before embedding them. "
          << "The query engine supports vector search using HNSW indexing "
          << "and full-text search using SQLite FTS5. Results are fused "
          << "with Reciprocal Rank Fusion for optimal relevance. "
          << "The memory system allows agents to store and retrieve "
          << "conversational context across sessions.\n";
    }

    ASSERT_GT(IngestDocument(doc_path, "text/plain"), 0);

    // Set up QueryPipeline
    LlmConfig llm_cfg;
    IntentClassifier classifier(llm_cfg);
    RRFFusion fusion;
    VectorSearcher vec_searcher(*vec_, *embedder_);
    BM25Searcher bm25_searcher(*store_);
    PostFilter post_filter(*store_);
    DegradationManager deg_mgr;
    SqlExecutorStub sql_stub;

    QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                           classifier, post_filter, deg_mgr, sql_stub);

    // Hybrid query
    QueryRequest req;
    req.query = "How does the SPC pipeline process documents?";
    req.namespace_name = "default";
    req.top_k = 5;
    req.timeout_ms = 30000;  // Allow more time for real inference
    req.Normalize();
    Status s = req.Validate();
    ASSERT_TRUE(s.ok()) << s.message();

    QueryResponse resp = pipeline.Execute(req);
    EXPECT_TRUE(!resp.has_error || resp.meta.degraded)
        << "Pipeline should succeed or degrade: " << resp.error_message;

    if (!resp.has_error) {
        EXPECT_GT(resp.results.size(), 0u) << "Should find SPC-related content";
        std::cout << "[HYBRID] Query returned " << resp.results.size()
                  << " results" << std::endl;
        if (!resp.results.empty()) {
            std::cout << "[HYBRID] Top result score=" << resp.results[0].score
                      << std::endl;
        }
    }
}

// ==================================================================
// Test 4: Multilingual document ingestion + cross-language search
// ==================================================================

TEST_F(RealSPCPipelineTest, MultilingualIngestAndSearch) {
    // Ingest Chinese document
    std::string zh_path = (test_dir_ / "zh_doc.txt").string();
    {
        std::ofstream f(zh_path);
        f << "机器学习是人工智能的一个分支，它使计算机系统能够从数据中学习。"
          << "深度学习使用多层神经网络来建模复杂的模式。"
          << "常见的算法包括梯度下降、反向传播和随机优化。"
          << "训练数据被分为训练集、验证集和测试集来评估模型性能。\n";
    }

    std::string zh_doc_id;
    ASSERT_GT(IngestDocument(zh_path, "text/plain", &zh_doc_id), 0);

    // Query in English for the same concept
    EmbeddingResult en_query;
    ASSERT_TRUE(embedder_->Embed("machine learning algorithms and neural networks", &en_query).ok());

    auto results = vec_->Search(en_query.vector.data(), /*top_k=*/3);
    ASSERT_GT(results.size(), 0u) << "Cross-language search should return results";

    // Verify top result is from Chinese document
    CortrixBlock blk;
    ASSERT_EQ(store_->block_get(results[0].first, blk), 0);
    EXPECT_EQ(blk.doc_id, zh_doc_id)
        << "English query about ML should find Chinese ML document (bge-m3 multilingual)";

    std::cout << "[MULTILINGUAL] EN query → ZH doc, distance="
              << results[0].second << std::endl;
}

// ==================================================================
// Test 5: Multiple documents - semantic discrimination
// ==================================================================

TEST_F(RealSPCPipelineTest, MultiDocSemanticDiscrimination) {
    // Create 3 documents on distinct topics
    std::string paths[3];
    std::string doc_ids[3];

    // Doc 0: Software engineering
    paths[0] = (test_dir_ / "software.txt").string();
    {
        std::ofstream f(paths[0]);
        f << "Software engineering involves systematic approaches to "
          << "developing reliable software systems. Version control with "
          << "Git enables collaborative development. Continuous integration "
          << "and deployment pipelines automate testing and release. "
          << "Code review practices improve quality and knowledge sharing.\n";
    }

    // Doc 1: Marine biology
    paths[1] = (test_dir_ / "marine.txt").string();
    {
        std::ofstream f(paths[1]);
        f << "Marine biology studies organisms in ocean ecosystems. "
          << "Coral reefs support extraordinary biodiversity and are "
          << "threatened by rising sea temperatures. Deep sea creatures "
          << "adapt to extreme pressure and darkness. Whale migration "
          << "patterns span thousands of kilometers across oceans.\n";
    }

    // Doc 2: Classical music
    paths[2] = (test_dir_ / "music.txt").string();
    {
        std::ofstream f(paths[2]);
        f << "Classical music from the Baroque and Romantic periods "
          << "features complex harmonic structures. Beethoven symphonies "
          << "expanded orchestral possibilities. Piano concertos by "
          << "Mozart demonstrate elegant melodic development. Opera "
          << "combines vocal performance with dramatic storytelling.\n";
    }

    for (int i = 0; i < 3; i++) {
        ASSERT_GT(IngestDocument(paths[i], "text/plain", &doc_ids[i]), 0);
    }

    // Query about each topic and verify correct doc ranks first
    struct TestCase {
        const char* query;
        int expected_doc_idx;
    };
    TestCase cases[] = {
        {"Git version control and CI/CD pipelines", 0},
        {"Ocean ecosystems and coral reef conservation", 1},
        {"Beethoven orchestral compositions", 2},
    };

    for (const auto& tc : cases) {
        EmbeddingResult qemb;
        ASSERT_TRUE(embedder_->Embed(tc.query, &qemb).ok());

        auto results = vec_->Search(qemb.vector.data(), /*top_k=*/5);
        ASSERT_GT(results.size(), 0u);

        CortrixBlock blk;
        ASSERT_EQ(store_->block_get(results[0].first, blk), 0);
        EXPECT_EQ(blk.doc_id, doc_ids[tc.expected_doc_idx])
            << "Query '" << tc.query << "' should match doc " << tc.expected_doc_idx;

        std::cout << "[DISCRIMINATION] \"" << tc.query << "\" → doc_id="
                  << blk.doc_id << " (expected " << doc_ids[tc.expected_doc_idx]
                  << ")" << std::endl;
    }
}

}  // namespace
}  // namespace cortrix
