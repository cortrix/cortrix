/// @file test_e2e_document_lifecycle.cpp
/// @brief E2E: Document lifecycle tests (upload → query → delete → verify)
///
/// Tests complete document lifecycle flows:
///   1. Full lifecycle: upload → store → query → delete → verify query returns empty
///   2. Directory import: create multiple docs → import → query all
///   3. Doc update flow: upload → update status → re-upload with new hash → verify
///   4. Block count verification: upload → check block_count matches actual blocks

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
// Common
#include "cortrix/common/block_types.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace cortrix::store;  // PHnsw / PhnswConfig / IndexStats live in cortrix::store

// ==================================================================
// Document Lifecycle Test Fixture
// ==================================================================

class DocumentLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_e2e_doclife_" + std::to_string(getpid()));
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

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
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

    /// Helper: create a document with blocks from text, returning doc_id
    std::string IngestDocument(const std::string& filename,
                               const std::string& text,
                               const std::string& hash) {
        // Create doc record
        CortrixDoc doc;
        doc.source_type = "http_upload";
        doc.source_path = filename;
        doc.content_hash = hash;
        doc.file_size = static_cast<int64_t>(text.size());
        doc.mime_type = "text/plain";
        if (store_->doc_create(doc) != 0) return {};  // [D-I6] doc_id is a ULID string now

        // Store blob
        blob_->store("default", doc.doc_id, text.data(), text.size());

        // Chunk + embed + store blocks
        ChunkConfig chunk_cfg;
        chunk_cfg.chunk_size = 256;
        chunk_cfg.chunk_overlap = 30;
        RecursiveChunker chunker(chunk_cfg);
        auto chunks = chunker.Chunk(text);

        std::vector<std::string> chunk_texts;
        for (const auto& c : chunks) chunk_texts.push_back(c.text);

        std::vector<EmbeddingResult> embeddings;
        embedder_->EmbedBatch(chunk_texts, &embeddings);

        BlockAssembler assembler;
        for (size_t i = 0; i < chunks.size(); ++i) {
            CortrixBlock block = assembler.Assemble(
                doc.doc_id, chunks[i], embeddings[i], kBlockFile);
            store_->block_insert(block);
            vec_->AddPoint(embeddings[i].vector.data(), block.block_id);
        }

        store_->doc_update_status(doc.doc_id, DocStatus::kReady);
        return doc.doc_id;
    }

    fs::path test_dir_;
    std::string db_path_, blob_dir_, vec_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
    std::unique_ptr<CortrixBlobLocal> blob_;
    std::unique_ptr<PHnsw> vec_;
    std::unique_ptr<OnnxEmbedder> embedder_;
};

// Full lifecycle: upload → store → FTS query → delete → verify query returns empty
TEST_F(DocumentLifecycleTest, FullLifecycle) {
    // Generate text long enough for multiple chunks
    std::string text;
    for (int i = 0; i < 10; ++i) {
        text += "Quantum computing uses qubits to perform calculations "
                "exponentially faster than classical computers for certain problems. "
                "Superposition and entanglement are key quantum phenomena. ";
    }

    std::string doc_id = IngestDocument("quantum.txt", text, "sha256_quantum_lifecycle");
    ASSERT_FALSE(doc_id.empty());  // [D-I6] doc_id is a ULID string now

    // Verify doc exists
    CortrixDoc fetched;
    ASSERT_EQ(store_->doc_get(doc_id, fetched), 0);
    EXPECT_EQ(fetched.status, DocStatus::kReady);

    // FTS query should find content
    {
        std::vector<SearchResult> results;
        ASSERT_EQ(store_->search_fulltext("quantum computing", 10, results), 0);
        ASSERT_GT(results.size(), 0u) << "FTS should find quantum content";
        // Verify at least one result belongs to our doc
        bool found_our_doc = false;
        for (const auto& r : results) {
            if (r.doc_id == doc_id) { found_our_doc = true; break; }
        }
        EXPECT_TRUE(found_our_doc);
    }

    // Delete document
    ASSERT_EQ(store_->block_delete_by_doc(doc_id), 0);
    ASSERT_EQ(store_->doc_delete(doc_id), 0);

    // Verify doc is gone
    CortrixDoc deleted_doc;
    EXPECT_NE(store_->doc_get(doc_id, deleted_doc), 0)
        << "Deleted doc should not be retrievable";

    // FTS query should return empty for this doc's content
    {
        std::vector<SearchResult> results;
        store_->search_fulltext("quantum computing", 10, results);
        for (const auto& r : results) {
            EXPECT_NE(r.doc_id, doc_id)
                << "Deleted doc blocks should not appear in FTS";
        }
    }
}

// Directory import: create multiple doc files → import all → query across docs
TEST_F(DocumentLifecycleTest, DirectoryImportQuery) {
    // Ingest 3 different documents with distinct content
    std::string text_ml;
    for (int i = 0; i < 8; ++i) {
        text_ml += "Machine learning models are trained on large datasets "
                   "using gradient descent optimization. Neural networks "
                   "consist of layers of interconnected neurons. ";
    }

    std::string text_db;
    for (int i = 0; i < 8; ++i) {
        text_db += "Database indexing improves query performance by creating "
                   "data structures that allow faster lookups. B-tree indexes "
                   "are commonly used in relational databases like PostgreSQL. ";
    }

    std::string text_net;
    for (int i = 0; i < 8; ++i) {
        text_net += "Network protocols define rules for data transmission. "
                    "TCP provides reliable ordered delivery of data streams. "
                    "UDP is used for low-latency applications like gaming. ";
    }

    std::string id_ml = IngestDocument("ml_intro.txt", text_ml, "sha256_ml_doc");
    std::string id_db = IngestDocument("db_guide.txt", text_db, "sha256_db_doc");
    std::string id_net = IngestDocument("net_basics.txt", text_net, "sha256_net_doc");

    ASSERT_FALSE(id_ml.empty());  // [D-I6] doc_id is a ULID string now
    ASSERT_FALSE(id_db.empty());
    ASSERT_FALSE(id_net.empty());

    // All 3 doc IDs should be distinct
    EXPECT_NE(id_ml, id_db);
    EXPECT_NE(id_db, id_net);
    EXPECT_NE(id_ml, id_net);

    // Verify total doc count
    int64_t count = 0;
    ASSERT_EQ(store_->doc_count(&count), 0);
    EXPECT_EQ(count, 3);

    // FTS query for ML content
    {
        std::vector<SearchResult> results;
        ASSERT_EQ(store_->search_fulltext("gradient descent", 10, results), 0);
        EXPECT_GT(results.size(), 0u);
        bool found = false;
        for (const auto& r : results) {
            if (r.doc_id == id_ml) { found = true; break; }
        }
        EXPECT_TRUE(found) << "ML doc should be found by gradient descent query";
    }

    // FTS query for DB content
    {
        std::vector<SearchResult> results;
        ASSERT_EQ(store_->search_fulltext("database indexing", 10, results), 0);
        EXPECT_GT(results.size(), 0u);
        bool found = false;
        for (const auto& r : results) {
            if (r.doc_id == id_db) { found = true; break; }
        }
        EXPECT_TRUE(found) << "DB doc should be found by database indexing query";
    }

    // FTS query for network content
    {
        std::vector<SearchResult> results;
        ASSERT_EQ(store_->search_fulltext("network protocols", 10, results), 0);
        EXPECT_GT(results.size(), 0u);
        bool found = false;
        for (const auto& r : results) {
            if (r.doc_id == id_net) { found = true; break; }
        }
        EXPECT_TRUE(found) << "Net doc should be found by network protocols query";
    }
}

// Doc update flow: upload → update status to error → re-upload with new hash → verify
TEST_F(DocumentLifecycleTest, DocUpdateFlow) {
    // First version upload
    CortrixDoc doc_v1;
    doc_v1.source_type = "http_upload";
    doc_v1.source_path = "report.txt";
    doc_v1.content_hash = "sha256_v1_original";
    doc_v1.file_size = 500;
    doc_v1.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc_v1), 0);
    std::string doc_id = doc_v1.doc_id;
    ASSERT_FALSE(doc_id.empty());  // [D-I6] doc_id is a ULID string now

    // Mark as ready
    ASSERT_EQ(store_->doc_update_status(doc_id, DocStatus::kReady), 0);

    // Verify it's ready
    CortrixDoc check;
    ASSERT_EQ(store_->doc_get(doc_id, check), 0);
    EXPECT_EQ(check.status, DocStatus::kReady);

    // Mark as stale (content changed externally)
    ASSERT_EQ(store_->doc_update_status(doc_id, DocStatus::kStale), 0);

    // Delete old blocks
    ASSERT_EQ(store_->block_delete_by_doc(doc_id), 0);

    // Delete old doc
    ASSERT_EQ(store_->doc_delete(doc_id), 0);

    // Re-upload with new hash (new version)
    CortrixDoc doc_v2;
    doc_v2.source_type = "http_upload";
    doc_v2.source_path = "report.txt";
    doc_v2.content_hash = "sha256_v2_updated";
    doc_v2.file_size = 600;
    doc_v2.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc_v2), 0);
    ASSERT_FALSE(doc_v2.doc_id.empty());

    // Verify new version exists with correct hash
    CortrixDoc fetched_v2;
    ASSERT_EQ(store_->doc_get(doc_v2.doc_id, fetched_v2), 0);
    EXPECT_EQ(fetched_v2.content_hash, "sha256_v2_updated");
    EXPECT_EQ(fetched_v2.file_size, 600);

    // Old hash should not be findable
    CortrixDoc old_lookup;
    EXPECT_NE(store_->doc_find_by_hash("sha256_v1_original", old_lookup), 0)
        << "Old hash should not exist after delete+re-upload";

    // New hash should be findable
    CortrixDoc new_lookup;
    ASSERT_EQ(store_->doc_find_by_hash("sha256_v2_updated", new_lookup), 0);
    EXPECT_EQ(new_lookup.doc_id, doc_v2.doc_id);
}

// Block count verification: upload → check block_count matches actual blocks stored
TEST_F(DocumentLifecycleTest, BlockCountVerification) {
    // Generate text for a predictable number of chunks
    std::string text;
    for (int i = 0; i < 15; ++i) {
        text += "Distributed systems require consensus algorithms to maintain "
                "consistency across replicas. Raft and Paxos are popular "
                "consensus protocols used in production systems. ";
    }

    std::string doc_id = IngestDocument("distributed.txt", text, "sha256_distributed");
    ASSERT_FALSE(doc_id.empty());  // [D-I6] doc_id is a ULID string now

    // Get the actual blocks stored for this doc
    std::vector<CortrixBlock> blocks;
    ASSERT_EQ(store_->block_get_by_doc(doc_id, blocks), 0);
    ASSERT_GT(blocks.size(), 0u) << "Should have at least one block";

    // Verify block count in total
    int64_t total_blocks = 0;
    ASSERT_EQ(store_->block_count(&total_blocks), 0);
    EXPECT_EQ(static_cast<size_t>(total_blocks), blocks.size())
        << "Total block count should match blocks for this doc (only doc in store)";

    // Verify each block references the correct doc_id
    for (const auto& block : blocks) {
        EXPECT_EQ(block.doc_id, doc_id)
            << "Block " << block.block_id << " should reference doc " << doc_id;
        EXPECT_GT(block.block_id, 0);
    }

    // Verify blocks have sequential chunk indices
    for (size_t i = 0; i < blocks.size(); ++i) {
        EXPECT_EQ(blocks[i].chunk_index, static_cast<int>(i))
            << "Block chunk_index should be sequential";
    }

    // Verify HNSW index has the same number of vectors
    EXPECT_EQ(vec_->GetStats().vector_count, blocks.size())
        << "HNSW index size should match number of blocks";
}

}  // namespace
}  // namespace cortrix
