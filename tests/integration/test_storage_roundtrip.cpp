#include <gtest/gtest.h>
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/cortrix_blob_local.h"
#include "cortrix/store/phnsw.h"
#include "cortrix/common/block_header.h"
#include <filesystem>
#include <random>

using namespace cortrix;
using namespace cortrix::store;  // PHnsw / PhnswConfig / IndexStats live in cortrix::store
namespace fs = std::filesystem;

class StorageRoundtripTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / ("cortrix_rt_test_" + std::to_string(rand()));
        fs::create_directories(test_dir_);

        db_path_ = (test_dir_ / "test.db").string();
        blob_dir_ = (test_dir_ / "blobs").string();
        vec_path_ = (test_dir_ / "vec").string();  // PHnsw uses a unit data dir (not a file)

        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);

        blob_ = std::make_unique<CortrixBlobLocal>(blob_dir_);

        PhnswConfig cfg;
        cfg.dim = 8;
        cfg.max_elements = 1000;
        vec_ = std::make_unique<PHnsw>(vec_path_, cfg);  // ctor runs Recover()
    }

    void TearDown() override {
        store_->Close();
        store_.reset();
        blob_.reset();
        vec_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    std::vector<float> RandomVec(int dim) {
        std::vector<float> v(dim);
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        float norm = 0;
        for (int i = 0; i < dim; ++i) { v[i] = dist(rng); norm += v[i]*v[i]; }
        norm = std::sqrt(norm);
        for (int i = 0; i < dim; ++i) v[i] /= norm;
        return v;
    }

    fs::path test_dir_;
    std::string db_path_, blob_dir_, vec_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
    std::unique_ptr<CortrixBlobLocal> blob_;
    std::unique_ptr<PHnsw> vec_;
};

// Test 1: Full pipeline: create doc -> store blob -> insert block -> search FTS -> cleanup
TEST_F(StorageRoundtripTest, FullPipeline) {
    // 1. Create document
    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = "/test/doc1.txt";
    doc.content_hash = "abc123";
    doc.file_size = 1024;
    doc.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc), 0);
    ASSERT_FALSE(doc.doc_id.empty());  // [D-I6] doc_id is a ULID string now

    // 2. Store blob
    std::string raw_data = "This is the raw document content for testing.";
    ASSERT_EQ(blob_->store("default", doc.doc_id, raw_data.data(), raw_data.size()), 0);

    // 3. Insert block with FTS content
    auto block_data = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL3,
                                  "searchable text content", "{\"doc_id\":\"" + doc.doc_id + "\"}", "");

    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.block_type = static_cast<int>(CortrixBlockType::kBlockFile);
    block.processing_level = static_cast<int>(ProcessingLevel::kLevelL3);
    block.content_text = "searchable text content";
    block.chunk_index = 0;
    block.data = block_data;
    ASSERT_EQ(store_->block_insert(block), 0);
    ASSERT_GT(block.block_id, 0);

    // 4. Add vector
    auto v = RandomVec(8);
    ASSERT_TRUE(vec_->AddPoint(v.data(), block.block_id).ok());

    // 5. Full-text search
    std::vector<SearchResult> fts_results;
    int ret = store_->search_fulltext("searchable", 10, fts_results);
    ASSERT_EQ(ret, 0);
    ASSERT_GE(fts_results.size(), 1u);
    EXPECT_EQ(fts_results[0].block_id, block.block_id);

    // 6. Vector search
    auto vec_results = vec_->Search(v.data(), /*top_k=*/1);
    ASSERT_EQ(vec_results.size(), 1u);
    EXPECT_EQ(vec_results[0].first, block.block_id);

    // 7. Load blob
    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("default", doc.doc_id, loaded), 0);
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), raw_data);

    // 8. Cleanup
    ASSERT_EQ(store_->block_delete_by_doc(doc.doc_id), 0);
    ASSERT_EQ(store_->doc_delete(doc.doc_id), 0);
    ASSERT_EQ(blob_->del("default", doc.doc_id), 0);
}

// Test 2: Cascade delete removes all blocks for a doc
TEST_F(StorageRoundtripTest, CascadeDeleteBlocks) {
    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = "/test/multi_block.txt";
    doc.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc), 0);

    // Insert 3 blocks
    for (int i = 0; i < 3; ++i) {
        std::string text = "Chunk " + std::to_string(i);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.block_type = 1;
        block.content_text = text;
        block.chunk_index = i;
        block.data = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL1, text, "", "");
        ASSERT_EQ(store_->block_insert(block), 0);
    }

    // Verify blocks exist
    std::vector<CortrixBlock> blocks;
    ASSERT_EQ(store_->block_get_by_doc(doc.doc_id, blocks), 0);
    EXPECT_EQ(blocks.size(), 3u);

    // Delete all blocks for doc
    ASSERT_EQ(store_->block_delete_by_doc(doc.doc_id), 0);

    blocks.clear();
    ASSERT_EQ(store_->block_get_by_doc(doc.doc_id, blocks), 0);
    EXPECT_EQ(blocks.size(), 0u);
}

// Test 3: Crash recovery - processing docs reset to pending
TEST_F(StorageRoundtripTest, CrashRecovery) {
    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = "/test/crash.txt";
    doc.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc), 0);

    // Simulate crash: set doc to processing
    ASSERT_EQ(store_->doc_update_status(doc.doc_id, DocStatus::kProcessing), 0);

    // Close and reopen (simulates crash recovery)
    store_->Close();
    store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
    ASSERT_EQ(store_->Open(), 0);  // RecoverCrashedDocs runs during Open

    // Doc should be reset to pending
    CortrixDoc recovered;
    ASSERT_EQ(store_->doc_get(doc.doc_id, recovered), 0);
    EXPECT_EQ(recovered.status, DocStatus::kPending);
}

// Test 3b: Crash recovery - deleting docs are fully deleted on recovery
TEST_F(StorageRoundtripTest, CrashRecoveryDeleting) {
    CortrixDoc doc;
    doc.source_type = "watch_dir";
    doc.source_path = "/test/deleting_crash.txt";
    doc.mime_type = "text/plain";
    ASSERT_EQ(store_->doc_create(doc), 0);

    // Add a block to the doc
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.content_text = "block content";
    block.data = {0x01, 0x02, 0x03};  // non-empty data to satisfy NOT NULL
    ASSERT_EQ(store_->block_insert(block), 0);

    // Simulate crash during deletion: set doc to kDeleting
    ASSERT_EQ(store_->doc_update_status(doc.doc_id, DocStatus::kDeleting), 0);

    // Close and reopen (simulates crash recovery)
    store_->Close();
    store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
    ASSERT_EQ(store_->Open(), 0);  // RecoverCrashedDocs runs during Open

    // Doc should be fully deleted (not just reset)
    CortrixDoc recovered;
    int rc = store_->doc_get(doc.doc_id, recovered);
    EXPECT_EQ(rc, -2);  // not found = fully deleted

    // Blocks should also be deleted
    std::vector<CortrixBlock> blocks;
    store_->block_get_by_doc(doc.doc_id, blocks);
    EXPECT_EQ(blocks.size(), 0u);
}

// Test 4: Vector index persistence across sessions
TEST_F(StorageRoundtripTest, VectorPersistence) {
    auto v1 = RandomVec(8);
    ASSERT_TRUE(vec_->AddPoint(v1.data(), 100).ok());
    ASSERT_TRUE(vec_->Snapshot().ok());
    vec_.reset();  // release the unit dir's WAL/lock before reopening (PHnsw is single-writer)

    // Reopen from the same unit dir — the ctor runs Recover() (snapshot + WAL replay).
    PhnswConfig cfg;
    cfg.dim = 8;
    cfg.max_elements = 1000;
    auto vec2 = std::make_unique<PHnsw>(vec_path_, cfg);
    EXPECT_EQ(vec2->GetStats().vector_count, 1u);

    auto results = vec2->Search(v1.data(), /*top_k=*/1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 100u);
}

// Test 5: Doc find_by_source returns correct doc
TEST_F(StorageRoundtripTest, FindBySource) {
    CortrixDoc doc;
    doc.source_type = "watch_dir";
    doc.source_path = "/data/reports/q4.pdf";
    doc.mime_type = "application/pdf";
    ASSERT_EQ(store_->doc_create(doc), 0);

    CortrixDoc found;
    ASSERT_EQ(store_->doc_find_by_source("watch_dir", "/data/reports/q4.pdf", found), 0);
    EXPECT_EQ(found.doc_id, doc.doc_id);
    EXPECT_EQ(found.source_path, "/data/reports/q4.pdf");
}
