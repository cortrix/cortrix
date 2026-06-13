/// @file test_cross_feature_e2e.cpp
/// @brief D3.5 Phase 5: Cross-Feature integration & E2E smoke tests
///
/// Tests the following cross-Feature data flows:
///   1. Upload → SPC → Store → Query (F01+F02+F03+F04+F07+F08)
///   2. Memory persistence across HTTP requests (F09+F10 critical fix verification)
///   3. Memory → Query → Inject full user journey (F07+F09+F10)
///   4. Document Upload → Store → Query via HTTP (F04+F01+F07+F10)
///   5. Namespace isolation across all subsystems

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>

#include "httplib.h"
#include <nlohmann/json.hpp>

// Storage layer (F01)
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/cortrix_blob_local.h"
#include "cortrix/store/phnsw.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"

// SPC pipeline (F03)
#include "cortrix/spc/document_parser.h"
#include "cortrix/spc/recursive_chunker.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/spc_manager.h"

// Query engine (F07+F08)
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/query_response.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/sql_executor.h"

// Memory system (F09)
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_writer.h"
#include "cortrix/memory/memory_searcher.h"
#include "cortrix/memory/memory_injector.h"
#include "cortrix/memory/memory_config.h"

// HTTP routes (F10+F12)
#include "cortrix/memory/memory_routes.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/server/routes/document_routes.h"
#include "cortrix/upload/upload_handler.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/config/config.h"
#include "cortrix/server/http_server.h"

// [wire⑤c] F05 NsPoolHarness replaces the MVP NamespaceManager +
// CortrixNamespaceManager (Register*Routes now take resource::INamespacePool&).
#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using cortrix::store::PHnsw;        // F01 PHnsw replaces MVP CortrixVectorHnswlib
using cortrix::store::PhnswConfig;

// ==================================================================
// Test 1: SPC → Store → Query Pipeline (no HTTP, direct API)
// Verifies: F03 (SPC) → F01 (Store) → F02 (Block) → F07 (Query)
// ==================================================================

class SpcStoreQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / ("cortrix_e2e_ssq_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);

        db_path_ = (test_dir_ / "cortrix.db").string();
        blob_dir_ = (test_dir_ / "blobs").string();
        vec_path_ = (test_dir_ / "vec").string();  // PHnsw uses a unit data dir (not a file)

        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);

        blob_ = std::make_unique<CortrixBlobLocal>(blob_dir_);

        PhnswConfig cfg;
        cfg.dim = 128;
        cfg.max_elements = 10000;
        vec_ = std::make_unique<PHnsw>(vec_path_, cfg);  // ctor runs Recover()

        embedder_ = std::make_unique<OnnxEmbedder>("stub.onnx", 128);
        ASSERT_TRUE(embedder_->Init().ok());

        // Create test document file
        doc_path_ = (test_dir_ / "test_document.txt").string();
        {
            std::ofstream f(doc_path_);
            // Generate enough text for multiple chunks at chunk_size=256
            for (int i = 0; i < 20; ++i) {
                f << "Section " << i << ": Cortrix is a semantic storage engine "
                  << "designed for AI agents. It processes documents through a "
                  << "pipeline called SPC (Split-Parse-Chunk). Documents are "
                  << "first parsed to extract text content, then chunked into "
                  << "smaller pieces for embedding. Each chunk is embedded "
                  << "into a vector space for similarity search. The query "
                  << "engine supports both vector search and full-text search "
                  << "(BM25), fused via RRF (Reciprocal Rank Fusion). "
                  << "This architecture allows agents to find relevant "
                  << "information quickly through natural language queries.\n\n";
            }
        }
    }

    void TearDown() override {
        store_->Close();
        store_.reset();
        blob_.reset();
        vec_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::string db_path_, blob_dir_, vec_path_, doc_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
    std::unique_ptr<CortrixBlobLocal> blob_;
    std::unique_ptr<PHnsw> vec_;
    std::unique_ptr<OnnxEmbedder> embedder_;
};

// Core E2E: Upload doc → Parse → Chunk → Embed → Store (SQLite+HNSW) → Query (BM25+Vector)
TEST_F(SpcStoreQueryTest, FullPipelineUploadToQuery) {
    // ===== Phase 1: SPC Pipeline (F03) =====
    // 1a. Parse document
    TxtParser parser;
    ParseResult parse_result;
    Status s = parser.Parse(doc_path_, "text/plain", &parse_result);
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_FALSE(parse_result.text.empty());

    // 1b. Chunk
    ChunkConfig chunk_cfg;
    chunk_cfg.chunk_size = 256;
    chunk_cfg.chunk_overlap = 30;
    RecursiveChunker chunker(chunk_cfg);
    auto chunks = chunker.Chunk(parse_result.text);
    ASSERT_GT(chunks.size(), 1u) << "Expected multiple chunks";

    // 1c. Embed all chunks
    std::vector<std::string> chunk_texts;
    for (const auto& c : chunks) chunk_texts.push_back(c.text);

    std::vector<EmbeddingResult> embeddings;
    s = embedder_->EmbedBatch(chunk_texts, &embeddings);
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_EQ(embeddings.size(), chunks.size());

    // ===== Phase 2: Store (F01+F02) =====
    // 2a. Create document record
    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = "test_document.txt";
    doc.content_hash = "sha256_e2e_test";
    doc.file_size = fs::file_size(doc_path_);
    doc.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc), 0);
    ASSERT_FALSE(doc.doc_id.empty());  // [D-I6] doc_id is a ULID string now

    // 2b. Store blob (raw content)
    std::string raw_content = parse_result.text;
    ASSERT_EQ(blob_->store("default", doc.doc_id,
              raw_content.data(), raw_content.size()), 0);

    // 2c. Assemble blocks and insert into store + HNSW
    BlockAssembler assembler;
    for (size_t i = 0; i < chunks.size(); ++i) {
        CortrixBlock block = assembler.Assemble(
            doc.doc_id, chunks[i], embeddings[i], kBlockFile);
        ASSERT_EQ(store_->block_insert(block), 0);
        ASSERT_GT(block.block_id, 0);

        // Insert vector into HNSW index
        ASSERT_TRUE(vec_->AddPoint(embeddings[i].vector.data(),
                  block.block_id).ok());
    }

    // 2d. Mark doc as ready
    ASSERT_EQ(store_->doc_update_status(doc.doc_id, DocStatus::kReady), 0);

    // ===== Phase 3: Query (F07+F08) =====
    // 3a. FTS query — search for "semantic storage"
    {
        std::vector<SearchResult> fts_results;
        int rc = store_->search_fulltext("semantic storage", 10, fts_results);
        ASSERT_EQ(rc, 0);
        ASSERT_GT(fts_results.size(), 0u) << "FTS should find 'semantic storage' in stored blocks";

        // Verify the result belongs to our document
        CortrixBlock result_block;
        ASSERT_EQ(store_->block_get(fts_results[0].block_id, result_block), 0);
        EXPECT_EQ(result_block.doc_id, doc.doc_id);
    }

    // 3b. Vector search — embed a query, find nearest
    {
        EmbeddingResult query_emb;
        s = embedder_->Embed("How does the query engine work?", &query_emb);
        ASSERT_TRUE(s.ok());

        auto vec_results = vec_->Search(query_emb.vector.data(), /*top_k=*/5);
        ASSERT_GT(vec_results.size(), 0u) << "Vector search should return results";

        // Verify all results are from our document
        for (const auto& vr : vec_results) {
            CortrixBlock blk;
            ASSERT_EQ(store_->block_get(vr.first, blk), 0);
            EXPECT_EQ(blk.doc_id, doc.doc_id);
        }
    }

    // 3c. QueryPipeline integration (BM25 + Vector + RRF fusion)
    {
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

        QueryRequest req;
        req.query = "What is the SPC pipeline?";
        req.namespace_name = "default";
        req.top_k = 5;
        req.timeout_ms = 5000;
        req.Normalize();
        s = req.Validate();
        ASSERT_TRUE(s.ok()) << s.message();

        QueryResponse resp = pipeline.Execute(req);
        // Either succeeds or degrades gracefully
        EXPECT_TRUE(!resp.has_error || resp.meta.degraded)
            << "Pipeline should succeed or degrade: " << resp.error_message;

        if (!resp.has_error) {
            EXPECT_GT(resp.results.size(), 0u) << "Should find SPC-related content";
        }
    }

    // ===== Phase 4: Verify blob roundtrip =====
    {
        std::vector<uint8_t> loaded;
        ASSERT_EQ(blob_->load("default", doc.doc_id, loaded), 0);
        std::string loaded_str(loaded.begin(), loaded.end());
        EXPECT_EQ(loaded_str, raw_content);
    }
}

// ==================================================================
// Test 2: Memory Persistence Across HTTP Requests
// Verifies CRITICAL Phase 2 fix: MemoryStore uses persistent SQLite
// Tests: F09 (Memory) + F10 (HTTP Routes) cross-request persistence
// ==================================================================

class MemoryPersistenceHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_e2e_mem_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        config_.ns.data_dir = tmp_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.memory.inject_recent_turns = 3;
        config_.memory.inject_max_tokens = 2000;
        config_.memory.default_ttl_seconds = 0;
        config_.memory.max_sessions_per_namespace = 100;
        config_.memory.max_interactions_per_session = 500;

        // [wire⑤c] F05 NS resource pool replaces the MVP NamespaceManager +
        // CortrixNamespaceManager (RegisterMemoryRoutes now takes INamespacePool&).
        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);

        auth_ = std::make_unique<ApiKeyAuth>();
        test_key_ = "test-e2e-key-memory";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "e2e-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        embedder_ = std::make_unique<OnnxEmbedder>("stub.onnx", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();

        spc_mgr_ = std::make_unique<TestSPCManager>();

        svr_ = std::make_unique<httplib::Server>();
        RegisterMemoryRoutes(*svr_, *auth_, harness_->ipool(), *spc_mgr_,
                             *embedder_, *classifier_, *fusion_,
                             config_.memory);

        port_ = svr_->bind_to_any_port("127.0.0.1");
        svr_thread_ = std::thread([this] {
            svr_->listen_after_bind();
        });

        // Wait for server
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/memory/sessions");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    // Fake SPCManager for memory write path
    class TestSPCManager : public SPCManager {
    public:
        TestSPCManager() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    CortrixConfig config_;
    fs::path tmp_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<TestSPCManager> spc_mgr_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string test_key_;
    int port_;
};

// CRITICAL: Verify memory sessions persist across separate HTTP requests.
// This directly tests the Phase 2 fix for ephemeral MemoryStore.
TEST_F(MemoryPersistenceHttpTest, SessionPersistsAcrossRequests) {
    httplib::Client cli("127.0.0.1", port_);

    // Ensure "default" namespace exists
    harness_->Admit("default");

    // REQUEST 1: Create session
    json create_body;
    create_body["namespace"] = "default";
    create_body["user_id"] = "e2e_user";
    create_body["title"] = "Persistence Test Session";

    auto res1 = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                          create_body.dump(), "application/json");
    ASSERT_TRUE(res1);
    ASSERT_EQ(res1->status, 201) << "Create session failed: " << res1->body;

    auto body1 = json::parse(res1->body);
    std::string session_id = body1["session_id"].get<std::string>();
    ASSERT_FALSE(session_id.empty());

    // REQUEST 2: Write interaction to the session (separate HTTP request)
    json write_body;
    write_body["namespace"] = "default";
    write_body["query_text"] = "What is the capital of France?";
    write_body["response_text"] = "The capital of France is Paris.";
    write_body["query_type"] = "chat";

    auto res2 = cli.Post(
        "/api/v1/memory/sessions/" + session_id + "/interactions",
        AuthHeaders(), write_body.dump(), "application/json");
    ASSERT_TRUE(res2);
    ASSERT_EQ(res2->status, 201) << "Write interaction failed: " << res2->body;

    // REQUEST 3: Get session detail (third separate HTTP request)
    // If MemoryStore is ephemeral, this would return 404.
    // MEM05: pass owner user_id for the ownership check.
    auto res3 = cli.Get(
        "/api/v1/memory/sessions/" + session_id + "?namespace=default&user_id=e2e_user",
        AuthHeaders());
    ASSERT_TRUE(res3);
    ASSERT_EQ(res3->status, 200)
        << "Session GET failed (ephemeral MemoryStore?): " << res3->body;

    auto body3 = json::parse(res3->body);
    EXPECT_EQ(body3["session"]["session_id"], session_id);
    EXPECT_EQ(body3["session"]["user_id"], "e2e_user");
    EXPECT_EQ(body3["session"]["title"], "Persistence Test Session");

    // Verify interactions are present
    ASSERT_TRUE(body3.contains("interactions"));
    ASSERT_GE(body3["interactions"].size(), 2u)
        << "Should have at least 2 interactions (user + assistant)";

    // REQUEST 4: List sessions (fourth separate HTTP request)
    // MEM05: list is user-isolated; pass the owner user_id.
    auto res4 = cli.Get("/api/v1/memory/sessions?namespace=default&user_id=e2e_user",
                         AuthHeaders());
    ASSERT_TRUE(res4);
    ASSERT_EQ(res4->status, 200) << "List sessions failed: " << res4->body;

    auto body4 = json::parse(res4->body);
    EXPECT_GE(body4["total_count"].get<int>(), 1);
    EXPECT_GE(body4["sessions"].size(), 1u);

    // Find our session in the list
    bool found = false;
    for (const auto& sess : body4["sessions"]) {
        if (sess["session_id"] == session_id) {
            found = true;
            EXPECT_EQ(sess["user_id"], "e2e_user");
            break;
        }
    }
    EXPECT_TRUE(found) << "Created session not found in session list";
}

// Verify that multiple sessions with interactions don't interfere
TEST_F(MemoryPersistenceHttpTest, MultipleSessionsIsolation) {
    httplib::Client cli("127.0.0.1", port_);
    harness_->Admit("default");

    // Create 2 sessions
    std::string sid1, sid2;
    for (int i = 0; i < 2; ++i) {
        json body;
        body["namespace"] = "default";
        body["user_id"] = "user_" + std::to_string(i);

        auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                             body.dump(), "application/json");
        ASSERT_TRUE(res);
        ASSERT_EQ(res->status, 201);
        auto j = json::parse(res->body);
        if (i == 0) sid1 = j["session_id"].get<std::string>();
        else sid2 = j["session_id"].get<std::string>();
    }

    // Write different content to each session
    auto writeInteraction = [&](const std::string& sid,
                                 const std::string& query,
                                 const std::string& response) {
        json body;
        body["namespace"] = "default";
        body["query_text"] = query;
        body["response_text"] = response;
        return cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                         AuthHeaders(), body.dump(), "application/json");
    };

    auto r1 = writeInteraction(sid1, "Session1 query about Paris",
                                     "Paris is the capital of France");
    ASSERT_TRUE(r1 && r1->status == 201);

    auto r2 = writeInteraction(sid2, "Session2 query about Tokyo",
                                     "Tokyo is the capital of Japan");
    ASSERT_TRUE(r2 && r2->status == 201);

    // Verify session 1 only has its own interactions (MEM05: owner = user_0).
    auto g1 = cli.Get("/api/v1/memory/sessions/" + sid1 + "?namespace=default&user_id=user_0",
                       AuthHeaders());
    ASSERT_TRUE(g1 && g1->status == 200);
    auto j1 = json::parse(g1->body);
    bool has_paris = false, has_tokyo = false;
    for (const auto& log : j1["interactions"]) {
        std::string content = log["content"].get<std::string>();
        if (content.find("Paris") != std::string::npos) has_paris = true;
        if (content.find("Tokyo") != std::string::npos) has_tokyo = true;
    }
    EXPECT_TRUE(has_paris) << "Session1 should contain Paris content";
    EXPECT_FALSE(has_tokyo) << "Session1 should NOT contain Tokyo content";

    // Verify session 2 only has its own interactions (MEM05: owner = user_1).
    auto g2 = cli.Get("/api/v1/memory/sessions/" + sid2 + "?namespace=default&user_id=user_1",
                       AuthHeaders());
    ASSERT_TRUE(g2 && g2->status == 200);
    auto j2 = json::parse(g2->body);
    has_paris = false;
    has_tokyo = false;
    for (const auto& log : j2["interactions"]) {
        std::string content = log["content"].get<std::string>();
        if (content.find("Paris") != std::string::npos) has_paris = true;
        if (content.find("Tokyo") != std::string::npos) has_tokyo = true;
    }
    EXPECT_FALSE(has_paris) << "Session2 should NOT contain Paris content";
    EXPECT_TRUE(has_tokyo) << "Session2 should contain Tokyo content";
}

// Verify inject endpoint returns recent context correctly
TEST_F(MemoryPersistenceHttpTest, InjectReturnsRecentContext) {
    httplib::Client cli("127.0.0.1", port_);
    harness_->Admit("default");

    // Create session
    json create_body;
    create_body["namespace"] = "default";
    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                         create_body.dump(), "application/json");
    ASSERT_TRUE(res && res->status == 201);
    std::string sid = json::parse(res->body)["session_id"].get<std::string>();

    // Write 5 turns
    for (int i = 0; i < 5; ++i) {
        json body;
        body["namespace"] = "default";
        body["query_text"] = "Turn " + std::to_string(i) + " question about AI";
        body["response_text"] = "Turn " + std::to_string(i) + " answer about AI";
        auto r = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                           AuthHeaders(), body.dump(), "application/json");
        ASSERT_TRUE(r && r->status == 201) << "Turn " << i << " write failed";
    }

    // Inject — should return last 3 turns (inject_recent_turns=3)
    json inject_body;
    inject_body["namespace"] = "default";
    inject_body["session_id"] = sid;

    auto inject_res = cli.Post("/api/v1/memory/inject", AuthHeaders(),
                                inject_body.dump(), "application/json");
    ASSERT_TRUE(inject_res);
    ASSERT_EQ(inject_res->status, 200) << inject_res->body;

    auto inject_json = json::parse(inject_res->body);
    EXPECT_EQ(inject_json["session_id"], sid);
    EXPECT_EQ(inject_json["turn_count"], 3);
    EXPECT_GT(inject_json["token_count_approx"].get<int>(), 0);

    std::string ctx = inject_json["context_text"].get<std::string>();
    EXPECT_FALSE(ctx.empty());

    // Should contain turns 2, 3, 4 (last 3)
    EXPECT_NE(ctx.find("Turn 2"), std::string::npos) << "Should contain turn 2";
    EXPECT_NE(ctx.find("Turn 4"), std::string::npos) << "Should contain turn 4";

    // Should NOT contain turns 0, 1 (older than inject_recent_turns)
    EXPECT_EQ(ctx.find("Turn 0"), std::string::npos) << "Should NOT contain turn 0";
}

// ==================================================================
// Test 3: Full HTTP E2E — Upload → Store → Query → Memory
// Verifies: F04 + F01 + F07 + F09 + F10 full user journey
// ==================================================================

class FullHttpE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_e2e_full_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        config_.ns.data_dir = tmp_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;
        config_.memory.inject_recent_turns = 3;
        config_.memory.inject_max_tokens = 2000;
        config_.memory.default_ttl_seconds = 0;
        config_.memory.max_sessions_per_namespace = 100;
        config_.memory.max_interactions_per_session = 500;

        // [wire⑤c] F05 NS resource pool replaces the MVP NamespaceManager +
        // CortrixNamespaceManager (all Register*Routes take INamespacePool&).
        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);

        auth_ = std::make_unique<ApiKeyAuth>();
        test_key_ = "test-full-e2e-key";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "full-e2e";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        embedder_ = std::make_unique<OnnxEmbedder>("stub.onnx", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();

        spc_mgr_ = std::make_unique<TestSPCManager>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_mgr_);

        svr_ = std::make_unique<httplib::Server>();

        // Register ALL route groups — this is the full server setup
        RegisterDocumentRoutes(*svr_, *handler_, harness_->ipool(), *auth_);
        RegisterQueryRoutes(*svr_, *auth_, harness_->ipool(), *embedder_,
                            *classifier_, *fusion_);
        RegisterMemoryRoutes(*svr_, *auth_, harness_->ipool(), *spc_mgr_,
                             *embedder_, *classifier_, *fusion_,
                             config_.memory);

        port_ = svr_->bind_to_any_port("127.0.0.1");
        svr_thread_ = std::thread([this] {
            svr_->listen_after_bind();
        });

        // Wait for server
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/memory/sessions");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    class TestSPCManager : public SPCManager {
    public:
        TestSPCManager() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    CortrixConfig config_;
    fs::path tmp_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<TestSPCManager> spc_mgr_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string test_key_;
    int port_;
};

// Upload via HTTP → verify doc created → query (empty result expected) → memory session
TEST_F(FullHttpE2ETest, UploadQueryMemoryJourney) {
    httplib::Client cli("127.0.0.1", port_);

    // Ensure "default" namespace exists
    harness_->Admit("default");

    // Step 1: Upload document via HTTP
    httplib::MultipartFormDataItems items = {
        {"file", "AI agents use semantic search to find relevant documents. "
                 "Vector embeddings enable similarity matching.",
         "ai_search.txt", "text/plain"},
    };

    auto upload_res = cli.Post("/api/v1/namespaces/default/documents",
                                AuthHeaders(), items);
    ASSERT_TRUE(upload_res);
    ASSERT_EQ(upload_res->status, 201) << upload_res->body;

    auto upload_body = json::parse(upload_res->body);
    std::string doc_id = upload_body["doc_id"].get<std::string>();  // [D-I6] ULID string
    EXPECT_FALSE(doc_id.empty());
    EXPECT_EQ(upload_body["status"], "pending");

    // Step 2: Check document status
    auto status_res = cli.Get(
        "/api/v1/namespaces/default/documents/" + doc_id + "/status",
        AuthHeaders());
    ASSERT_TRUE(status_res);
    EXPECT_EQ(status_res->status, 200);

    // Step 3: Query — document is pending (not yet processed), expect empty or degraded
    json query_body;
    query_body["query"] = "semantic search";
    query_body["namespace"] = "default";
    query_body["top_k"] = 5;

    auto query_res = cli.Post("/api/v1/query", AuthHeaders(),
                               query_body.dump(), "application/json");
    ASSERT_TRUE(query_res);
    // Empty namespace: 200 with empty results or 503 degraded — both acceptable
    EXPECT_TRUE(query_res->status == 200 || query_res->status == 503)
        << "Unexpected status: " << query_res->status << " body: " << query_res->body;

    // Step 4: Create memory session and log the interaction
    json session_body;
    session_body["namespace"] = "default";
    session_body["user_id"] = "agent_001";

    auto sess_res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                              session_body.dump(), "application/json");
    ASSERT_TRUE(sess_res && sess_res->status == 201);
    std::string sid = json::parse(sess_res->body)["session_id"].get<std::string>();

    // Step 5: Log the query+response as an interaction
    json interact_body;
    interact_body["namespace"] = "default";
    interact_body["query_text"] = "semantic search";
    interact_body["response_text"] = "Found 0 results (document still processing)";
    interact_body["query_type"] = "search";
    interact_body["latency_ms"] = 42;

    auto interact_res = cli.Post(
        "/api/v1/memory/sessions/" + sid + "/interactions",
        AuthHeaders(), interact_body.dump(), "application/json");
    ASSERT_TRUE(interact_res && interact_res->status == 201);

    // Step 6: Verify the full journey is recorded (MEM05: owner = agent_001).
    auto detail_res = cli.Get(
        "/api/v1/memory/sessions/" + sid + "?namespace=default&user_id=agent_001",
        AuthHeaders());
    ASSERT_TRUE(detail_res && detail_res->status == 200);
    auto detail_body = json::parse(detail_res->body);
    EXPECT_EQ(detail_body["session"]["user_id"], "agent_001");
    EXPECT_GE(detail_body["interactions"].size(), 2u);

    // Step 7: Inject context
    json inject_body;
    inject_body["namespace"] = "default";
    inject_body["session_id"] = sid;

    auto inject_res = cli.Post("/api/v1/memory/inject", AuthHeaders(),
                                inject_body.dump(), "application/json");
    ASSERT_TRUE(inject_res && inject_res->status == 200);
    auto inject_json = json::parse(inject_res->body);
    EXPECT_GE(inject_json["turn_count"].get<int>(), 1);
    EXPECT_FALSE(inject_json["context_text"].get<std::string>().empty());
}

// Verify error response format consistency across all route groups
TEST_F(FullHttpE2ETest, ErrorFormatConsistency) {
    httplib::Client cli("127.0.0.1", port_);

    // 1. Query route — invalid JSON
    auto r1 = cli.Post("/api/v1/query", AuthHeaders(),
                        "not json", "application/json");
    ASSERT_TRUE(r1);
    auto j1 = json::parse(r1->body);
    EXPECT_TRUE(j1.contains("error"));
    EXPECT_TRUE(j1["error"].contains("code"));
    EXPECT_TRUE(j1["error"].contains("message"));
    EXPECT_TRUE(j1["error"].contains("timestamp"));

    // 2. Memory route — missing namespace
    json mem_body;
    mem_body["query"] = "test";
    auto r2 = cli.Post("/api/v1/memory/search", AuthHeaders(),
                         mem_body.dump(), "application/json");
    ASSERT_TRUE(r2);
    auto j2 = json::parse(r2->body);
    EXPECT_TRUE(j2.contains("error"));
    EXPECT_TRUE(j2["error"].contains("code"));
    EXPECT_TRUE(j2["error"].contains("timestamp"));

    // 3. Auth failure — no token
    auto r3 = cli.Post("/api/v1/memory/sessions",
                         json({{"namespace", "default"}}).dump(),
                         "application/json");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->status, 401);
    auto j3 = json::parse(r3->body);
    EXPECT_TRUE(j3.contains("error"));
    EXPECT_TRUE(j3["error"].contains("timestamp"));
}

// ==================================================================
// Test 4: Namespace Isolation Across All Subsystems
// Verifies data doesn't leak between namespaces in store + memory
// ==================================================================

class NamespaceIsolationE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_e2e_nsiso_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        config_.ns.data_dir = tmp_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;
        config_.memory.inject_recent_turns = 3;
        config_.memory.inject_max_tokens = 2000;
        config_.memory.default_ttl_seconds = 0;
        config_.memory.max_sessions_per_namespace = 100;
        config_.memory.max_interactions_per_session = 500;

        // [wire⑤c] F05 NS resource pool replaces the MVP NamespaceManager +
        // CortrixNamespaceManager (RegisterMemoryRoutes now takes INamespacePool&).
        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);

        auth_ = std::make_unique<ApiKeyAuth>();
        test_key_ = "test-ns-iso-key";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "ns-iso";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        embedder_ = std::make_unique<OnnxEmbedder>("stub.onnx", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();
        spc_mgr_ = std::make_unique<TestSPCManager>();

        svr_ = std::make_unique<httplib::Server>();
        RegisterMemoryRoutes(*svr_, *auth_, harness_->ipool(), *spc_mgr_,
                             *embedder_, *classifier_, *fusion_,
                             config_.memory);

        port_ = svr_->bind_to_any_port("127.0.0.1");
        svr_thread_ = std::thread([this] {
            svr_->listen_after_bind();
        });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/memory/sessions");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    class TestSPCManager : public SPCManager {
    public:
        TestSPCManager() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    CortrixConfig config_;
    fs::path tmp_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<TestSPCManager> spc_mgr_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string test_key_;
    int port_;
};

// Memory sessions in namespace A should not appear in namespace B
TEST_F(NamespaceIsolationE2ETest, MemorySessionNamespaceIsolation) {
    httplib::Client cli("127.0.0.1", port_);

    harness_->Admit("project_alpha");
    harness_->Admit("project_beta");

    // Create session in project_alpha
    json alpha_body;
    alpha_body["namespace"] = "project_alpha";
    alpha_body["user_id"] = "alice";
    auto r_alpha = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                             alpha_body.dump(), "application/json");
    ASSERT_TRUE(r_alpha && r_alpha->status == 201);
    std::string sid_alpha = json::parse(r_alpha->body)["session_id"].get<std::string>();

    // Write interaction in project_alpha
    json w_alpha;
    w_alpha["namespace"] = "project_alpha";
    w_alpha["query_text"] = "Alpha secret project data";
    w_alpha["response_text"] = "Alpha confidential response";
    auto wr_alpha = cli.Post(
        "/api/v1/memory/sessions/" + sid_alpha + "/interactions",
        AuthHeaders(), w_alpha.dump(), "application/json");
    ASSERT_TRUE(wr_alpha && wr_alpha->status == 201);

    // Create session in project_beta
    json beta_body;
    beta_body["namespace"] = "project_beta";
    beta_body["user_id"] = "bob";
    auto r_beta = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                            beta_body.dump(), "application/json");
    ASSERT_TRUE(r_beta && r_beta->status == 201);

    // List sessions in project_beta — should NOT see alpha's session
    // (MEM05: bob lists his own beta sessions).
    auto list_beta = cli.Get("/api/v1/memory/sessions?namespace=project_beta&user_id=bob",
                              AuthHeaders());
    ASSERT_TRUE(list_beta && list_beta->status == 200);
    auto list_body = json::parse(list_beta->body);

    for (const auto& sess : list_body["sessions"]) {
        EXPECT_NE(sess["session_id"], sid_alpha)
            << "Alpha's session should NOT appear in beta's session list";
    }

    // List sessions in project_alpha — should see alpha's session
    // (MEM05: alice lists her own alpha sessions).
    auto list_alpha = cli.Get("/api/v1/memory/sessions?namespace=project_alpha&user_id=alice",
                               AuthHeaders());
    ASSERT_TRUE(list_alpha && list_alpha->status == 200);
    auto list_alpha_body = json::parse(list_alpha->body);
    EXPECT_GE(list_alpha_body["total_count"].get<int>(), 1);

    bool found = false;
    for (const auto& sess : list_alpha_body["sessions"]) {
        if (sess["session_id"] == sid_alpha) found = true;
    }
    EXPECT_TRUE(found) << "Alpha's session should appear in alpha's list";
}

// Delete session in one namespace should not affect another
TEST_F(NamespaceIsolationE2ETest, DeleteDoesNotCrossNamespace) {
    httplib::Client cli("127.0.0.1", port_);

    harness_->Admit("ns_x");
    harness_->Admit("ns_y");

    // Create sessions in both namespaces
    auto createSession = [&](const std::string& ns) -> std::string {
        json body;
        body["namespace"] = ns;
        body["user_id"] = "alice";  // MEM05: sessions are user-scoped, user_id required
        auto r = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                           body.dump(), "application/json");
        return json::parse(r->body)["session_id"].get<std::string>();
    };

    std::string sid_x = createSession("ns_x");
    std::string sid_y = createSession("ns_y");

    // Delete session in ns_x
    auto del = cli.Delete(
        "/api/v1/memory/sessions/" + sid_x + "?namespace=ns_x&user_id=alice",
        AuthHeaders());
    ASSERT_TRUE(del && del->status == 200);

    // Verify ns_y session is still intact
    auto get_y = cli.Get(
        "/api/v1/memory/sessions/" + sid_y + "?namespace=ns_y&user_id=alice",
        AuthHeaders());
    ASSERT_TRUE(get_y);
    EXPECT_EQ(get_y->status, 200) << "ns_y session should survive ns_x deletion";
}

}  // namespace
}  // namespace cortrix
