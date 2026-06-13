/// @file test_e2e_concurrent.cpp
/// @brief D4 E2E: Concurrency tests for thread safety
///
/// Tests concurrent access patterns:
///   1. Concurrent uploads: 5 threads each creating a doc → all succeed
///   2. Concurrent queries: 5 threads querying simultaneously → all get results
///   3. Concurrent memory writes: 3 threads writing to different sessions → isolation

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <filesystem>
#include <atomic>

#include <nlohmann/json.hpp>

// Storage
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/cortrix_blob_local.h"
#include "cortrix/store/phnsw.h"
// SPC
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/recursive_chunker.h"
// Memory
#include "cortrix/memory/memory_store.h"
#include "cortrix/common/block_types.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace cortrix::store;  // PHnsw / PhnswConfig / IndexStats live in cortrix::store

// ==================================================================
// Concurrent Test Fixture
// ==================================================================

class ConcurrentTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_e2e_conc_" + std::to_string(getpid()));
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
    std::string db_path_, blob_dir_, vec_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
    std::unique_ptr<CortrixBlobLocal> blob_;
    std::unique_ptr<PHnsw> vec_;
    std::unique_ptr<OnnxEmbedder> embedder_;
};

// 5 threads each creating a document concurrently → all succeed, no crash
TEST_F(ConcurrentTest, ConcurrentUploads) {
    const int kThreadCount = 5;
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreadCount; ++t) {
        threads.emplace_back([&, t]() {
            CortrixDoc doc;
            doc.source_type = "http_upload";
            doc.source_path = "concurrent_" + std::to_string(t) + ".txt";
            doc.content_hash = "sha256_conc_" + std::to_string(t);
            doc.file_size = 100 + t;
            doc.mime_type = "text/plain";

            int rc = store_->doc_create(doc);
            if (rc == 0 && !doc.doc_id.empty()) {  // [D-I6] doc_id is a ULID string now
                success_count.fetch_add(1);
            } else {
                failure_count.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(success_count.load(), kThreadCount)
        << "All " << kThreadCount << " concurrent creates should succeed";
    EXPECT_EQ(failure_count.load(), 0)
        << "No concurrent creates should fail";

    // Verify total doc count
    int64_t count = 0;
    ASSERT_EQ(store_->doc_count(&count), 0);
    EXPECT_EQ(count, kThreadCount);
}

// Seed some data, then 5 threads query FTS simultaneously → all get results
TEST_F(ConcurrentTest, ConcurrentQueries) {
    // Seed: create a document with blocks for FTS
    std::string text;
    for (int i = 0; i < 10; ++i) {
        text += "Artificial intelligence and deep learning have transformed "
                "natural language processing and computer vision tasks. "
                "Transformer architectures enable efficient parallel processing. ";
    }

    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = "ai_overview.txt";
    doc.content_hash = "sha256_ai_conc_query";
    doc.file_size = static_cast<int64_t>(text.size());
    doc.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc), 0);

    // Chunk + embed + store
    ChunkConfig chunk_cfg;
    chunk_cfg.chunk_size = 256;
    chunk_cfg.chunk_overlap = 30;
    RecursiveChunker chunker(chunk_cfg);
    auto chunks = chunker.Chunk(text);

    std::vector<std::string> chunk_texts;
    for (const auto& c : chunks) chunk_texts.push_back(c.text);

    std::vector<EmbeddingResult> embeddings;
    ASSERT_TRUE(embedder_->EmbedBatch(chunk_texts, &embeddings).ok());

    BlockAssembler assembler;
    for (size_t i = 0; i < chunks.size(); ++i) {
        CortrixBlock block = assembler.Assemble(
            doc.doc_id, chunks[i], embeddings[i], kBlockFile);
        ASSERT_EQ(store_->block_insert(block), 0);
        ASSERT_TRUE(vec_->AddPoint(embeddings[i].vector.data(),
                  block.block_id).ok());
    }
    store_->doc_update_status(doc.doc_id, DocStatus::kReady);

    // Concurrent FTS queries
    const int kQueryThreads = 5;
    std::atomic<int> query_success{0};
    std::vector<std::thread> threads;
    std::vector<std::string> queries = {
        "artificial intelligence", "deep learning", "natural language",
        "computer vision", "transformer"
    };

    for (int t = 0; t < kQueryThreads; ++t) {
        threads.emplace_back([&, t]() {
            std::vector<SearchResult> results;
            int rc = store_->search_fulltext(queries[t], 10, results);
            if (rc == 0 && !results.empty()) {
                query_success.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(query_success.load(), kQueryThreads)
        << "All " << kQueryThreads << " concurrent FTS queries should return results";

    // Concurrent vector queries
    std::atomic<int> vec_success{0};
    threads.clear();

    for (int t = 0; t < kQueryThreads; ++t) {
        threads.emplace_back([&, t]() {
            EmbeddingResult query_emb;
            Status s = embedder_->Embed(queries[t], &query_emb);
            if (!s.ok()) return;

            auto results = vec_->Search(query_emb.vector.data(), /*top_k=*/5);
            if (!results.empty()) {
                vec_success.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(vec_success.load(), kQueryThreads)
        << "All concurrent vector queries should return results";
}

// 3 threads writing to different memory sessions → isolation maintained
TEST_F(ConcurrentTest, ConcurrentMemoryWrites) {
    // Create MemoryStore
    MemoryStore mem_store(*store_);
    std::string mem_db_path = (test_dir_ / "memory.db").string();
    ASSERT_TRUE(mem_store.Init(mem_db_path).ok());

    const int kSessionCount = 3;
    const int kInteractionsPerSession = 5;
    std::vector<std::string> session_ids(kSessionCount);
    std::atomic<int> success_count{0};

    // Create sessions first (sequentially — just metadata setup)
    for (int s = 0; s < kSessionCount; ++s) {
        MemorySession session;
        session.namespace_name = "default";
        session.user_id = "user_" + std::to_string(s);
        session.title = "Session " + std::to_string(s);
        Status st = mem_store.SessionCreate(session);
        ASSERT_TRUE(st.ok()) << "Session " << s << " create failed: " << st.message();
        session_ids[s] = session.session_id;
    }

    // Concurrent writes: each thread writes interactions to its own session
    std::vector<std::thread> threads;
    for (int s = 0; s < kSessionCount; ++s) {
        threads.emplace_back([&, s]() {
            int local_success = 0;
            for (int i = 0; i < kInteractionsPerSession; ++i) {
                InteractionLog log;
                log.session_id = session_ids[s];
                log.namespace_name = "default";
                log.user_id = "user_" + std::to_string(s);
                log.role = "user";
                log.content = "Session" + std::to_string(s) +
                              " interaction " + std::to_string(i);
                log.query_type = "chat";
                log.status = "success";

                Status st = mem_store.InteractionInsert(log);
                if (st.ok()) local_success++;
            }
            success_count.fetch_add(local_success);
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(success_count.load(), kSessionCount * kInteractionsPerSession)
        << "All concurrent memory writes should succeed";

    // Verify session isolation: each session has exactly its own interactions
    for (int s = 0; s < kSessionCount; ++s) {
        std::vector<InteractionLog> interactions;
        Status st = mem_store.InteractionListBySession(session_ids[s], interactions);
        ASSERT_TRUE(st.ok());
        EXPECT_EQ(interactions.size(),
                   static_cast<size_t>(kInteractionsPerSession))
            << "Session " << s << " should have exactly "
            << kInteractionsPerSession << " interactions";

        // All interactions in this session should belong to this session's user
        std::string expected_prefix = "Session" + std::to_string(s);
        for (const auto& log : interactions) {
            EXPECT_EQ(log.session_id, session_ids[s]);
            EXPECT_NE(log.content.find(expected_prefix), std::string::npos)
                << "Interaction in session " << s
                << " should contain '" << expected_prefix << "', got: "
                << log.content;
        }
    }
}

}  // namespace
}  // namespace cortrix
