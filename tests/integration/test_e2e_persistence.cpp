/// @file test_e2e_persistence.cpp
/// @brief D4 E2E: Persistence tests (data survives close/reopen cycle)
///
/// Tests that stored data survives a store close + reopen:
///   1. Store data survives restart: create docs/blocks → close → reopen → verify
///   2. Memory data survives restart: create session/interactions → close → reopen → verify

#include <gtest/gtest.h>
#include <filesystem>

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
// Persistence Test Fixture
// ==================================================================

class PersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_e2e_persist_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);

        db_path_ = (test_dir_ / "cortrix.db").string();
        blob_dir_ = (test_dir_ / "blobs").string();
        vec_path_ = (test_dir_ / "vec").string();  // PHnsw uses a unit data dir (not a file)
        mem_db_path_ = (test_dir_ / "memory.db").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::string db_path_, blob_dir_, vec_path_, mem_db_path_;
};

// Store data survives close + reopen cycle
TEST_F(PersistenceTest, StoreDataSurvivesRestart) {
    const int kDocCount = 3;
    std::vector<std::string> doc_ids;  // [D-I6] doc_id is a ULID string now
    size_t total_blocks = 0;

    // ===== Phase 1: Create data and close =====
    {
        auto store = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store->Open(), 0);

        auto blob = std::make_unique<CortrixBlobLocal>(blob_dir_);

        PhnswConfig cfg;
        cfg.dim = 128;
        cfg.max_elements = 10000;
        auto vec = std::make_unique<PHnsw>(vec_path_, cfg);  // ctor runs Recover()

        auto embedder = std::make_unique<OnnxEmbedder>("stub.onnx", 128);
        ASSERT_TRUE(embedder->Init().ok());

        // Create documents with blocks
        for (int d = 0; d < kDocCount; ++d) {
            std::string text;
            for (int i = 0; i < 8; ++i) {
                text += "Document " + std::to_string(d) + " section " +
                        std::to_string(i) + ": This is test content for "
                        "persistence verification across store restarts. "
                        "Each document contains unique identifiable content. ";
            }

            CortrixDoc doc;
            doc.source_type = "http_upload";
            doc.source_path = "persist_doc_" + std::to_string(d) + ".txt";
            doc.content_hash = "sha256_persist_" + std::to_string(d);
            doc.file_size = static_cast<int64_t>(text.size());
            doc.mime_type = "text/plain";
            ASSERT_EQ(store->doc_create(doc), 0);
            doc_ids.push_back(doc.doc_id);

            // Store blob
            ASSERT_EQ(blob->store("default", doc.doc_id,
                      text.data(), text.size()), 0);

            // Chunk + embed + store blocks
            ChunkConfig chunk_cfg;
            chunk_cfg.chunk_size = 256;
            chunk_cfg.chunk_overlap = 30;
            RecursiveChunker chunker(chunk_cfg);
            auto chunks = chunker.Chunk(text);

            std::vector<std::string> chunk_texts;
            for (const auto& c : chunks) chunk_texts.push_back(c.text);

            std::vector<EmbeddingResult> embeddings;
            ASSERT_TRUE(embedder->EmbedBatch(chunk_texts, &embeddings).ok());

            BlockAssembler assembler;
            for (size_t i = 0; i < chunks.size(); ++i) {
                CortrixBlock block = assembler.Assemble(
                    doc.doc_id, chunks[i], embeddings[i], kBlockFile);
                ASSERT_EQ(store->block_insert(block), 0);
                ASSERT_TRUE(vec->AddPoint(embeddings[i].vector.data(),
                          block.block_id).ok());
            }

            store->doc_update_status(doc.doc_id, DocStatus::kReady);
            total_blocks += chunks.size();
        }

        // Snapshot the PHnsw index before closing (truncates the WAL).
        ASSERT_TRUE(vec->Snapshot().ok());

        // Close store explicitly
        store->Close();
        // All unique_ptrs go out of scope here (vec's dtor releases the WAL lock
        // before Phase 2 reopens the same unit dir — PHnsw is single-writer).
    }

    // ===== Phase 2: Reopen and verify =====
    {
        auto store = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store->Open(), 0);

        auto blob = std::make_unique<CortrixBlobLocal>(blob_dir_);

        PhnswConfig cfg;
        cfg.dim = 128;
        cfg.max_elements = 10000;
        // Reopen the same unit dir — the ctor runs Recover() (snapshot + WAL replay).
        auto vec = std::make_unique<PHnsw>(vec_path_, cfg);

        // Verify doc count
        int64_t count = 0;
        ASSERT_EQ(store->doc_count(&count), 0);
        EXPECT_EQ(count, kDocCount) << "Doc count should survive restart";

        // Verify each doc exists and is ready
        for (int d = 0; d < kDocCount; ++d) {
            CortrixDoc doc;
            ASSERT_EQ(store->doc_get(doc_ids[d], doc), 0)
                << "Doc " << d << " (id=" << doc_ids[d]
                << ") should exist after restart";
            EXPECT_EQ(doc.status, DocStatus::kReady);
            EXPECT_EQ(doc.content_hash,
                       "sha256_persist_" + std::to_string(d));

            // Verify blocks for this doc
            std::vector<CortrixBlock> blocks;
            ASSERT_EQ(store->block_get_by_doc(doc_ids[d], blocks), 0);
            EXPECT_GT(blocks.size(), 0u)
                << "Doc " << d << " should have blocks after restart";

            // Verify blob roundtrip
            std::vector<uint8_t> blob_data;
            ASSERT_EQ(blob->load("default", doc_ids[d], blob_data), 0)
                << "Blob for doc " << d << " should survive restart";
            EXPECT_GT(blob_data.size(), 0u);
        }

        // Verify total block count
        int64_t block_count = 0;
        ASSERT_EQ(store->block_count(&block_count), 0);
        EXPECT_EQ(static_cast<size_t>(block_count), total_blocks)
            << "Total block count should survive restart";

        // Verify PHnsw index reloaded with the same vectors
        EXPECT_EQ(vec->GetStats().vector_count, total_blocks)
            << "PHnsw index should reload with same number of vectors";

        // Verify FTS still works
        {
            std::vector<SearchResult> results;
            ASSERT_EQ(store->search_fulltext("persistence verification", 10,
                      results), 0);
            EXPECT_GT(results.size(), 0u)
                << "FTS should return results after restart";
        }

        store->Close();
    }
}

// Memory session + interactions survive close + reopen cycle
TEST_F(PersistenceTest, MemoryDataSurvivesRestart) {
    std::string session_id;
    const int kInteractionCount = 5;

    // ===== Phase 1: Create memory data and close =====
    {
        auto store = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store->Open(), 0);

        MemoryStore mem_store(*store);
        ASSERT_TRUE(mem_store.Init(mem_db_path_).ok());

        // Create session
        MemorySession session;
        session.namespace_name = "default";
        session.user_id = "persist_user";
        session.title = "Persistence Test Session";
        ASSERT_TRUE(mem_store.SessionCreate(session).ok());
        session_id = session.session_id;
        ASSERT_FALSE(session_id.empty());

        // Write interactions
        for (int i = 0; i < kInteractionCount; ++i) {
            InteractionLog log;
            log.session_id = session_id;
            log.namespace_name = "default";
            log.user_id = "persist_user";
            log.role = (i % 2 == 0) ? "user" : "assistant";
            log.content = "Persist interaction " + std::to_string(i) +
                          " with unique content marker_" + std::to_string(i);
            log.query_type = "chat";
            log.status = "success";
            log.latency_ms = 10 + i;
            ASSERT_TRUE(mem_store.InteractionInsert(log).ok())
                << "Interaction " << i << " insert failed";
        }

        // Verify data exists before close
        MemorySession check_session;
        ASSERT_TRUE(mem_store.SessionGet(session_id, check_session).ok());
        EXPECT_EQ(check_session.user_id, "persist_user");

        store->Close();
    }

    // ===== Phase 2: Reopen and verify =====
    {
        auto store = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store->Open(), 0);

        MemoryStore mem_store(*store);
        ASSERT_TRUE(mem_store.Init(mem_db_path_).ok());

        // Verify session survives restart
        MemorySession session;
        Status s = mem_store.SessionGet(session_id, session);
        ASSERT_TRUE(s.ok())
            << "Session should survive restart: " << s.message();
        EXPECT_EQ(session.session_id, session_id);
        EXPECT_EQ(session.user_id, "persist_user");
        EXPECT_EQ(session.title, "Persistence Test Session");
        EXPECT_EQ(session.namespace_name, "default");

        // Verify interactions survive restart
        std::vector<InteractionLog> interactions;
        s = mem_store.InteractionListBySession(session_id, interactions);
        ASSERT_TRUE(s.ok()) << "Interaction list should work after restart";
        EXPECT_EQ(interactions.size(),
                   static_cast<size_t>(kInteractionCount))
            << "All interactions should survive restart";

        // Verify interaction content integrity
        for (int i = 0; i < kInteractionCount; ++i) {
            const auto& log = interactions[i];
            EXPECT_EQ(log.session_id, session_id);
            EXPECT_EQ(log.namespace_name, "default");
            std::string marker = "marker_" + std::to_string(i);
            EXPECT_NE(log.content.find(marker), std::string::npos)
                << "Interaction " << i << " should contain '" << marker
                << "', got: " << log.content;
        }

        // Verify session list works
        std::vector<MemorySession> sessions;
        s = mem_store.SessionList("default", 10, 0, sessions);
        ASSERT_TRUE(s.ok());
        EXPECT_GE(sessions.size(), 1u);

        bool found = false;
        for (const auto& sess : sessions) {
            if (sess.session_id == session_id) {
                found = true;
                EXPECT_EQ(sess.user_id, "persist_user");
                break;
            }
        }
        EXPECT_TRUE(found) << "Session should be in list after restart";

        // Verify interaction count
        int64_t int_count = 0;
        s = mem_store.InteractionCount(session_id, &int_count);
        ASSERT_TRUE(s.ok());
        EXPECT_EQ(int_count, kInteractionCount)
            << "Interaction count should match after restart";

        store->Close();
    }
}

}  // namespace
}  // namespace cortrix
