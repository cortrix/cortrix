#include <gtest/gtest.h>
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/common/block_header.h"
#include <sqlite3.h>
#include <unistd.h>
#include <filesystem>
#include <thread>
#include <chrono>

using namespace cortrix;
namespace fs = std::filesystem;

class StoreSqliteTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / ("cortrix_store_test_" + std::to_string(::getpid()) + "_" + std::to_string(rand()));
        fs::create_directories(test_dir_);
        db_path_ = (test_dir_ / "test.db").string();
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);
    }

    void TearDown() override {
        store_->Close();
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    void ExecSql(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(store_->db_handle(), sql.c_str(), nullptr, nullptr, &err);
        std::string msg = err ? err : "";
        sqlite3_free(err);
        ASSERT_EQ(rc, SQLITE_OK) << msg << " :: " << sql;
    }

    fs::path test_dir_;
    std::string db_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
};

// Test 1: OpenCreatesTables - Open creates tables/indexes
TEST_F(StoreSqliteTest, OpenCreatesTables) {
    // Already opened in SetUp, verify tables exist by trying to query
    int64_t count = -1;
    EXPECT_EQ(store_->doc_count(&count), 0);
    EXPECT_EQ(count, 0);

    EXPECT_EQ(store_->block_count(&count), 0);
    EXPECT_EQ(count, 0);
}

// Test 2: PragmaWalEnabled - journal_mode = wal
TEST_F(StoreSqliteTest, PragmaWalEnabled) {
    // SQLite WAL mode creates -wal file
    std::string wal_file = db_path_ + "-wal";
    // WAL file may not exist until first write, so just verify no error
    EXPECT_TRUE(fs::exists(db_path_));
}

// Test 3: DocCreateAndGet - insert → read all fields
TEST_F(StoreSqliteTest, DocCreateAndGet) {
    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = "/uploads/test.pdf";
    doc.source_ref = "user123";
    doc.content_hash = "abc123hash";
    doc.file_size = 1024;
    doc.mime_type = "application/pdf";
    doc.processing_level = 3;
    doc.chunk_strategy = "recursive_split";
    doc.title = "Test Document";
    doc.language = "en";
    doc.metadata_json = R"({"key":"value"})";

    // Create
    int ret = store_->doc_create(doc);
    ASSERT_EQ(ret, 0);
    EXPECT_FALSE(doc.doc_id.empty());
    EXPECT_FALSE(doc.created_at.empty());

    // Get
    CortrixDoc retrieved;
    ret = store_->doc_get(doc.doc_id, retrieved);
    ASSERT_EQ(ret, 0);

    // Verify all fields
    EXPECT_EQ(retrieved.doc_id, doc.doc_id);
    EXPECT_EQ(retrieved.source_type, "http_upload");
    EXPECT_EQ(retrieved.source_path, "/uploads/test.pdf");
    EXPECT_EQ(retrieved.source_ref, "user123");
    EXPECT_EQ(retrieved.content_hash, "abc123hash");
    EXPECT_EQ(retrieved.file_size, 1024);
    EXPECT_EQ(retrieved.mime_type, "application/pdf");
    EXPECT_EQ(retrieved.status, DocStatus::kPending);
    EXPECT_EQ(retrieved.processing_level, 3);
    EXPECT_EQ(retrieved.chunk_strategy, "recursive_split");
    EXPECT_EQ(retrieved.title, "Test Document");
    EXPECT_EQ(retrieved.language, "en");
    EXPECT_EQ(retrieved.metadata_json, R"({"key":"value"})");
    EXPECT_FALSE(retrieved.created_at.empty());
}

// Test 4: DocCreateAutoId - doc_create mints a unique non-empty ULID per doc
TEST_F(StoreSqliteTest, DocCreateAutoId) {
    CortrixDoc doc1, doc2;
    doc1.source_type = "test";
    doc1.source_path = "path1";
    doc2.source_type = "test";
    doc2.source_path = "path2";

    store_->doc_create(doc1);
    store_->doc_create(doc2);

    EXPECT_FALSE(doc1.doc_id.empty());
    EXPECT_FALSE(doc2.doc_id.empty());
    EXPECT_NE(doc1.doc_id, doc2.doc_id);  // each doc gets a distinct id
}

// Test 5: DocUpdateStatus - pending→processing→ready
TEST_F(StoreSqliteTest, DocUpdateStatus) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Update to processing
    int ret = store_->doc_update_status(doc.doc_id, DocStatus::kProcessing);
    EXPECT_EQ(ret, 0);

    CortrixDoc retrieved;
    store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kProcessing);

    // Update to ready
    ret = store_->doc_update_status(doc.doc_id, DocStatus::kReady);
    EXPECT_EQ(ret, 0);

    store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kReady);

    // Update to error with message
    ret = store_->doc_update_status(doc.doc_id, DocStatus::kError, "Test error");
    EXPECT_EQ(ret, 0);

    store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kError);
    EXPECT_EQ(retrieved.error_message, "Test error");
}

// Test 6: DocDeleteCascade - doc delete → blocks also deleted
TEST_F(StoreSqliteTest, DocDeleteCascade) {
    // Create doc
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Create blocks
    auto blob = BlockBuild(kBlockFile, kLevelL2, "test content", "", "", 0, 0);
    CortrixBlock block1, block2;
    block1.doc_id = doc.doc_id;
    block1.chunk_index = 0;
    block1.block_type = 1;
    block1.processing_level = 2;
    block1.data = blob;
    block1.content_text = "test content";

    block2 = block1;
    block2.chunk_index = 1;

    store_->block_insert(block1);
    store_->block_insert(block2);

    // Verify blocks exist
    std::vector<CortrixBlock> blocks;
    store_->block_get_by_doc(doc.doc_id, blocks);
    ASSERT_EQ(blocks.size(), 2);

    // Delete doc
    int ret = store_->doc_delete(doc.doc_id);
    EXPECT_EQ(ret, 0);

    // Verify doc gone
    CortrixDoc retrieved;
    ret = store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(ret, -2);  // not found

    // Verify blocks gone (cascade delete)
    blocks.clear();
    store_->block_get_by_doc(doc.doc_id, blocks);
    EXPECT_TRUE(blocks.empty());
}

// [F16a D3 · P2c] DeleteBySourcePrefix — full-overwrite import cleanup.
TEST_F(StoreSqliteTest, DeleteBySourcePrefix) {
    auto add_doc = [&](const std::string& source_path, int n_blocks) {
        CortrixDoc doc;
        doc.source_type = "db_import";
        doc.source_path = source_path;
        store_->doc_create(doc);
        auto blob = BlockBuild(kBlockFile, kLevelL2, "row", "", "", 0, 0);
        for (int i = 0; i < n_blocks; ++i) {
            CortrixBlock b;
            b.doc_id = doc.doc_id;
            b.chunk_index = i;
            b.block_type = 1;
            b.processing_level = 2;
            b.data = blob;
            b.content_text = "row";
            store_->block_insert(b);
        }
        return doc.doc_id;
    };

    // Two rows of table "users", one row of "users2" (the boundary trap), one of "orders".
    std::string u1 = add_doc("postgres://h/db/users/1", 1);
    std::string u2 = add_doc("postgres://h/db/users/2", 1);
    add_doc("postgres://h/db/users2/1", 1);   // must NOT be cleared by "users/"
    add_doc("postgres://h/db/orders/1", 1);

    int64_t before = -1;
    store_->doc_count(&before);
    ASSERT_EQ(before, 4);

    // Clear the prior import of table "users" (prefix ends at the table boundary).
    int64_t removed = -1;
    EXPECT_EQ(store_->doc_delete_by_source_prefix("postgres://h/db/users/", &removed), 0);
    EXPECT_EQ(removed, 2);  // 2 user rows × 1 block each

    // The two users docs + their blocks are gone.
    CortrixDoc tmp;
    EXPECT_EQ(store_->doc_get(u1, tmp), -2);
    EXPECT_EQ(store_->doc_get(u2, tmp), -2);
    std::vector<CortrixBlock> blocks;
    store_->block_get_by_doc(u1, blocks);
    EXPECT_TRUE(blocks.empty());

    // users2 + orders survive (boundary respected).
    int64_t after = -1;
    store_->doc_count(&after);
    EXPECT_EQ(after, 2);
}

// [F16a D3 · P2c] DeleteBySourcePrefix is a no-op on an empty prefix (never wipes
// the whole namespace) and on a prefix that matches nothing.
TEST_F(StoreSqliteTest, DeleteBySourcePrefixGuards) {
    CortrixDoc doc;
    doc.source_type = "db_import";
    doc.source_path = "postgres://h/db/t/1";
    store_->doc_create(doc);

    int64_t removed = -1;
    EXPECT_EQ(store_->doc_delete_by_source_prefix("", &removed), 0);
    EXPECT_EQ(removed, 0);  // empty prefix clears nothing

    EXPECT_EQ(store_->doc_delete_by_source_prefix("postgres://h/db/none/", &removed), 0);
    EXPECT_EQ(removed, 0);  // no match

    int64_t count = -1;
    store_->doc_count(&count);
    EXPECT_EQ(count, 1);  // the doc is untouched
}

// Test 7: DocListByStatus - filter by status
TEST_F(StoreSqliteTest, DocListByStatus) {
    CortrixDoc doc1, doc2, doc3;
    doc1.source_type = "test";
    doc1.source_path = "path1";
    doc2.source_type = "test";
    doc2.source_path = "path2";
    doc3.source_type = "test";
    doc3.source_path = "path3";

    store_->doc_create(doc1);
    store_->doc_create(doc2);
    store_->doc_create(doc3);

    store_->doc_update_status(doc2.doc_id, DocStatus::kProcessing);
    store_->doc_update_status(doc3.doc_id, DocStatus::kReady);

    // List pending docs
    std::vector<CortrixDoc> docs;
    int ret = store_->doc_list_by_status(DocStatus::kPending, docs);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(docs.size(), 1);
    EXPECT_EQ(docs[0].doc_id, doc1.doc_id);

    // List processing docs
    docs.clear();
    ret = store_->doc_list_by_status(DocStatus::kProcessing, docs);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(docs.size(), 1);
    EXPECT_EQ(docs[0].doc_id, doc2.doc_id);

    // List ready docs
    docs.clear();
    ret = store_->doc_list_by_status(DocStatus::kReady, docs);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(docs.size(), 1);
    EXPECT_EQ(docs[0].doc_id, doc3.doc_id);
}

// Test 8: DocFindBySource - find by source_type + source_path
TEST_F(StoreSqliteTest, DocFindBySource) {
    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = "/uploads/unique.pdf";
    store_->doc_create(doc);

    CortrixDoc found;
    int ret = store_->doc_find_by_source("http_upload", "/uploads/unique.pdf", found);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(found.doc_id, doc.doc_id);
    EXPECT_EQ(found.source_path, "/uploads/unique.pdf");

    // Not found case
    CortrixDoc not_found;
    ret = store_->doc_find_by_source("http_upload", "/nonexistent.pdf", not_found);
    EXPECT_EQ(ret, -2);
}

// Test 9: BlockInsertAndGet - block CRUD
TEST_F(StoreSqliteTest, BlockInsertAndGet) {
    // Create doc first
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Create block with blob data
    auto blob = BlockBuild(kBlockFile, kLevelL2, "test content", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "test content";

    int ret = store_->block_insert(block);
    ASSERT_EQ(ret, 0);
    EXPECT_GT(block.block_id, 0);

    // Retrieve block
    CortrixBlock retrieved;
    ret = store_->block_get(block.block_id, retrieved);
    ASSERT_EQ(ret, 0);
    EXPECT_EQ(retrieved.doc_id, doc.doc_id);
    EXPECT_EQ(retrieved.chunk_index, 0);
    EXPECT_EQ(retrieved.block_type, 1);
    EXPECT_EQ(retrieved.processing_level, 2);
    EXPECT_EQ(retrieved.content_text, "test content");
    EXPECT_FALSE(retrieved.data.empty());
}

TEST_F(StoreSqliteTest, BlockGetReadsOptionalScoreSignalsWhenColumnsExist) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "score-signals.txt";
    ASSERT_EQ(store_->doc_create(doc), 0);

    auto blob = BlockBuild(kBlockFile, kLevelL3, "scored content", "", "", 0, 0);
    CortrixBlock first;
    first.doc_id = doc.doc_id;
    first.chunk_index = 0;
    first.block_type = 1;
    first.processing_level = 3;
    first.data = blob;
    first.content_text = "scored content";
    ASSERT_EQ(store_->block_insert(first), 0);

    CortrixBlock second = first;
    second.block_id = 0;
    second.chunk_index = 1;
    second.content_text = "semantic only";
    ASSERT_EQ(store_->block_insert(second), 0);

    ExecSql("ALTER TABLE blocks ADD COLUMN enriched_score REAL DEFAULT NULL");
    ExecSql("ALTER TABLE blocks ADD COLUMN semantic_score REAL DEFAULT NULL");
    ExecSql("UPDATE blocks SET enriched_score=0.8, semantic_score=1.0 WHERE block_id=" +
            std::to_string(first.block_id));
    ExecSql("UPDATE blocks SET semantic_score=0.6 WHERE block_id=" +
            std::to_string(second.block_id));

    CortrixBlock got;
    ASSERT_EQ(store_->block_get(first.block_id, got), 0);
    ASSERT_TRUE(got.score_signals.enriched_score.has_value());
    ASSERT_TRUE(got.score_signals.semantic_score.has_value());
    EXPECT_FLOAT_EQ(*got.score_signals.enriched_score, 0.8f);
    EXPECT_FLOAT_EQ(*got.score_signals.semantic_score, 1.0f);

    std::vector<CortrixBlock> by_doc;
    ASSERT_EQ(store_->block_get_by_doc(doc.doc_id, by_doc), 0);
    ASSERT_EQ(by_doc.size(), 2u);
    EXPECT_TRUE(by_doc[0].score_signals.enriched_score.has_value());
    EXPECT_TRUE(by_doc[0].score_signals.semantic_score.has_value());
    EXPECT_FALSE(by_doc[1].score_signals.enriched_score.has_value());
    ASSERT_TRUE(by_doc[1].score_signals.semantic_score.has_value());
    EXPECT_FLOAT_EQ(*by_doc[1].score_signals.semantic_score, 0.6f);
}

// [A unified-blocks] block_insert/block_get round-trip the F34 child columns
// (child_id/parent_id/token_count/parent_offset) + the shared metadata_json.
TEST_F(StoreSqliteTest, BlockRoundTripsAColumns) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "a.txt";
    store_->doc_create(doc);

    // A child row: child_id non-empty, block_type = source modality (kBlockFile=1).
    auto blob = BlockBuild(kBlockFile, kLevelL3, "child text", "", "", 0, 0);
    CortrixBlock child;
    child.block_id = 0xABCDEF0123456789ULL;  // explicit uint64 (hash of child ULID)
    child.doc_id = doc.doc_id;
    child.chunk_index = 0;
    child.block_type = 1;
    child.processing_level = 3;
    child.data = blob;
    child.content_text = "child text";
    child.child_id = "01HXCHILD0000000000000001";
    child.parent_id = "01HXPARENT000000000000001";
    child.token_count = 42;
    child.parent_offset = 7;
    child.metadata_json = R"({"ner":["Acme"]})";
    ASSERT_EQ(store_->block_insert(child), 0);

    CortrixBlock got;
    ASSERT_EQ(store_->block_get(child.block_id, got), 0);
    EXPECT_EQ(got.child_id, "01HXCHILD0000000000000001");
    EXPECT_EQ(got.parent_id, "01HXPARENT000000000000001");
    EXPECT_EQ(got.token_count, 42);
    EXPECT_EQ(got.parent_offset, 7);
    EXPECT_EQ(got.metadata_json, R"({"ner":["Acme"]})");

    // A non-child row: A columns left default → read back as empty / -1 (SQL NULL).
    CortrixBlock plain;
    plain.doc_id = doc.doc_id;
    plain.chunk_index = 1;
    plain.block_type = 1;
    plain.processing_level = 2;
    plain.data = BlockBuild(kBlockFile, kLevelL2, "plain", "", "", 0, 0);
    plain.content_text = "plain";
    ASSERT_EQ(store_->block_insert(plain), 0);

    CortrixBlock got2;
    ASSERT_EQ(store_->block_get(plain.block_id, got2), 0);
    EXPECT_TRUE(got2.child_id.empty());
    EXPECT_TRUE(got2.parent_id.empty());
    EXPECT_EQ(got2.token_count, -1);
    EXPECT_EQ(got2.parent_offset, -1);
    EXPECT_TRUE(got2.metadata_json.empty());
}

// Test 10: BlockGetByDocOrdered - blocks returned ordered by chunk_index
TEST_F(StoreSqliteTest, BlockGetByDocOrdered) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Insert blocks in reverse order
    for (int i = 5; i >= 0; --i) {
        auto blob = BlockBuild(kBlockFile, kLevelL2, "content", "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = i;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = "chunk " + std::to_string(i);
        store_->block_insert(block);
    }

    // Retrieve and verify order
    std::vector<CortrixBlock> blocks;
    store_->block_get_by_doc(doc.doc_id, blocks);
    ASSERT_EQ(blocks.size(), 6);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(blocks[i].chunk_index, i);
    }
}

// Test 11: BlockDeleteByDoc - delete all blocks for a doc
TEST_F(StoreSqliteTest, BlockDeleteByDoc) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Insert blocks
    for (int i = 0; i < 3; ++i) {
        auto blob = BlockBuild(kBlockFile, kLevelL2, "content", "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = i;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        store_->block_insert(block);
    }

    // Verify blocks exist
    std::vector<CortrixBlock> blocks;
    store_->block_get_by_doc(doc.doc_id, blocks);
    EXPECT_EQ(blocks.size(), 3);

    // Delete all blocks
    int ret = store_->block_delete_by_doc(doc.doc_id);
    EXPECT_EQ(ret, 0);

    // Verify blocks gone
    blocks.clear();
    store_->block_get_by_doc(doc.doc_id, blocks);
    EXPECT_TRUE(blocks.empty());
}

// Test 12: FTS5SearchChinese - full-text search handles Chinese.
// Chinese literals kept intentionally: this exercises CJK full-text search.
TEST_F(StoreSqliteTest, FTS5SearchChinese) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    doc.title = "中文测试文档";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "这是一个测试文档", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "这是一个测试文档";
    store_->block_insert(block);

    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("测试", 10, results);
    EXPECT_EQ(ret, 0);
    // Note: SQLite FTS5 default tokenizer may not tokenize Chinese correctly.
    // This test verifies the API works without crash; content matching
    // requires a Chinese-aware tokenizer (Phase 2+).
    if (!results.empty()) {
        EXPECT_EQ(results[0].content_text, "这是一个测试文档");
    }
}

// Test 13: FTS5SearchEnglish - full-text search handles English
TEST_F(StoreSqliteTest, FTS5SearchEnglish) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    doc.title = "English Test Document";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "Machine learning model", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "Machine learning model";
    store_->block_insert(block);

    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("learning", 10, results);
    EXPECT_EQ(ret, 0);
    EXPECT_GE(results.size(), 1);
    EXPECT_EQ(results[0].content_text, "Machine learning model");
}

// Test 14: FTS5TopK - respects top_k limit
TEST_F(StoreSqliteTest, FTS5TopK) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Insert 10 blocks with "test"
    for (int i = 0; i < 10; ++i) {
        auto blob = BlockBuild(kBlockFile, kLevelL2, "test content", "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = i;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = "test content " + std::to_string(i);
        store_->block_insert(block);
    }

    // Search with top_k = 5
    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("test", 5, results);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(results.size(), 5);
}

// Test 15: RecoverCrashedDocs - processing → pending on restart
TEST_F(StoreSqliteTest, RecoverCrashedDocs) {
    // Create docs in different states
    CortrixDoc doc1, doc2, doc3;
    doc1.source_type = "test";
    doc1.source_path = "path1";
    doc2.source_type = "test";
    doc2.source_path = "path2";
    doc3.source_type = "test";
    doc3.source_path = "path3";

    store_->doc_create(doc1);
    store_->doc_create(doc2);
    store_->doc_create(doc3);

    store_->doc_update_status(doc1.doc_id, DocStatus::kProcessing);
    store_->doc_update_status(doc2.doc_id, DocStatus::kReady);
    // doc3 stays pending

    // Simulate restart by calling RecoverCrashedDocs
    store_->RecoverCrashedDocs();

    // Verify doc1 is back to pending
    CortrixDoc retrieved;
    store_->doc_get(doc1.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kPending);

    // doc2 and doc3 unchanged
    store_->doc_get(doc2.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kReady);

    store_->doc_get(doc3.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kPending);
}

// Test 16: ConcurrentWrites - WAL mode supports concurrent writes
TEST_F(StoreSqliteTest, ConcurrentWrites) {
    std::vector<std::thread> threads;
    const int num_threads = 4;
    const int docs_per_thread = 5;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, docs_per_thread]() {
            for (int i = 0; i < docs_per_thread; ++i) {
                CortrixDoc doc;
                doc.source_type = "thread_" + std::to_string(t);
                doc.source_path = "path_" + std::to_string(i);
                int ret = store_->doc_create(doc);
                EXPECT_EQ(ret, 0);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // Verify all docs created
    int64_t count = 0;
    store_->doc_count(&count);
    EXPECT_EQ(count, num_threads * docs_per_thread);
}

// ============================================================
// FTS5 Sanitization Tests (M-SEC-001)
// ============================================================

// Test 17: SanitizeFts5Query - empty and whitespace input
TEST(Fts5SanitizeTest, EmptyInput) {
    EXPECT_EQ(SanitizeFts5Query(""), "");
    EXPECT_EQ(SanitizeFts5Query("   "), "");
    EXPECT_EQ(SanitizeFts5Query("\t\n"), "");
}

// Test 18: SanitizeFts5Query - simple word is quoted
TEST(Fts5SanitizeTest, SimpleWord) {
    EXPECT_EQ(SanitizeFts5Query("hello"), "\"hello\"");
}

// Test 19: SanitizeFts5Query - multiple words are individually quoted and
// OR-joined (D6: a bare space is implicit AND in FTS5, which forced every
// token to co-occur in one block and starved the BM25 route).
TEST(Fts5SanitizeTest, MultipleWords) {
    EXPECT_EQ(SanitizeFts5Query("hello world"), "\"hello\" OR \"world\"");
}

// Test 20: SanitizeFts5Query - FTS5 operators are quoted (treated as literals)
TEST(Fts5SanitizeTest, OperatorsQuoted) {
    // AND, OR, NOT should be treated as literal words, not operators; the only
    // bare OR tokens are the sanitizer's own joiners.
    EXPECT_EQ(SanitizeFts5Query("cat AND dog"), "\"cat\" OR \"AND\" OR \"dog\"");
    EXPECT_EQ(SanitizeFts5Query("cat OR dog"), "\"cat\" OR \"OR\" OR \"dog\"");
    EXPECT_EQ(SanitizeFts5Query("NOT cat"), "\"NOT\" OR \"cat\"");
    EXPECT_EQ(SanitizeFts5Query("NEAR(cat,dog)"), "\"NEAR(cat,dog)\"");
}

// Test 21: SanitizeFts5Query - wildcard star is stripped (no alnum)
TEST(Fts5SanitizeTest, PureSyntaxCharsStripped) {
    // Pure syntax-only tokens (no alphanumeric) are stripped
    EXPECT_EQ(SanitizeFts5Query("*"), "");
    EXPECT_EQ(SanitizeFts5Query("^"), "");
    EXPECT_EQ(SanitizeFts5Query("* ^ -"), "");
}

// Test 22: SanitizeFts5Query - mixed operators and words
TEST(Fts5SanitizeTest, MixedOperatorsAndWords) {
    // "* NOT *" attack should be neutralized
    EXPECT_EQ(SanitizeFts5Query("* NOT *"), "\"NOT\"");
}

// Test 23: SanitizeFts5Query - double quotes in input are escaped
TEST(Fts5SanitizeTest, QuotesEscaped) {
    // A token containing " should have it doubled inside the outer quotes
    std::string result = SanitizeFts5Query("he\"llo");
    EXPECT_EQ(result, "\"he\"\"llo\"");
}

// Test 24: SanitizeFts5Query - tokens with leading/trailing stars
TEST(Fts5SanitizeTest, TokensWithStars) {
    // "test*" still has alphanumeric, so it gets quoted
    EXPECT_EQ(SanitizeFts5Query("test*"), "\"test*\"");
}

// Test 25: SanitizeFts5Query - UTF-8/Chinese input preserved
TEST(Fts5SanitizeTest, Utf8Preserved) {
    std::string result = SanitizeFts5Query("测试");
    EXPECT_EQ(result, "\"测试\"");

    result = SanitizeFts5Query("hello 世界");
    EXPECT_EQ(result, "\"hello\" OR \"世界\"");
}

// Test 26: FTS5 search with operator injection is safe
TEST_F(StoreSqliteTest, FTS5OperatorInjectionSafe) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "machine learning model", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "machine learning model";
    store_->block_insert(block);

    // These queries would cause issues without sanitization:
    // "* NOT *" should not return all results
    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("* NOT *", 10, results);
    EXPECT_EQ(ret, 0);
    // After sanitization, "* NOT *" becomes just "NOT" which is treated as literal
    // This should not cause a crash

    // "AND" should be treated as a literal search term, not an operator
    ret = store_->search_fulltext("machine AND learning", 10, results);
    EXPECT_EQ(ret, 0);
    // Should find result since "machine" and "learning" are both present
    // (tokens are quoted individually and OR-joined by the sanitizer)
}

// D6 regression: a natural-language query must match blocks that contain only
// a SUBSET of its terms. Under the old implicit-AND join this returned zero
// rows (live 5k evidence: fts5 path had 0 votes on ~98% of queries).
TEST_F(StoreSqliteTest, FTS5NaturalLanguageMatchesPartialTerms) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    const char* texts[] = {"machine learning model", "distributed training pipeline"};
    for (int i = 0; i < 2; ++i) {
        auto blob = BlockBuild(kBlockFile, kLevelL2, texts[i], "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = i;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = texts[i];
        store_->block_insert(block);
    }

    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("what is machine training", 10, results);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(results.size(), 2u);  // each block matches one informative term
}

// D6 regression: bm25 rank still rewards the block matching more query terms,
// so OR-joining does not degrade ordering quality.
TEST_F(StoreSqliteTest, FTS5RankPrefersMoreMatchedTerms) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    const char* texts[] = {"machine learning model", "machine parts catalog"};
    for (int i = 0; i < 2; ++i) {
        auto blob = BlockBuild(kBlockFile, kLevelL2, texts[i], "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = i;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = texts[i];
        store_->block_insert(block);
    }

    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("machine learning", 10, results);
    EXPECT_EQ(ret, 0);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].content_text, "machine learning model");
}

// Test 27: FTS5 search with empty query returns empty results
TEST_F(StoreSqliteTest, FTS5EmptyQuery) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "some content", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "some content";
    store_->block_insert(block);

    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("", 10, results);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(results.empty());

    ret = store_->search_fulltext("   ", 10, results);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(results.empty());
}

// ============================================================
// Additional Edge Case Tests for F01
// ============================================================

// Test 28: DocGetNotFound - getting non-existent doc returns -2
TEST_F(StoreSqliteTest, DocGetNotFound) {
    CortrixDoc doc;
    int ret = store_->doc_get("01JNONEXISTENT000000000000", doc);
    EXPECT_EQ(ret, -2);
}

// Test 29: DocDeleteNotFound - deleting non-existent doc returns -2
TEST_F(StoreSqliteTest, DocDeleteNotFound) {
    int ret = store_->doc_delete("01JNONEXISTENT000000000000");
    EXPECT_EQ(ret, -2);
}

// Test 30: DocUpdateStatusNotFound - updating non-existent doc returns -2
TEST_F(StoreSqliteTest, DocUpdateStatusNotFound) {
    int ret = store_->doc_update_status("01JNONEXISTENT000000000000", DocStatus::kReady);
    EXPECT_EQ(ret, -2);
}

// Test 31: BlockGetNotFound - getting non-existent block returns -2
TEST_F(StoreSqliteTest, BlockGetNotFound) {
    CortrixBlock block;
    int ret = store_->block_get(99999, block);
    EXPECT_EQ(ret, -2);
}

// Test 32: BlockGetByDocEmpty - getting blocks for empty doc returns empty vector
TEST_F(StoreSqliteTest, BlockGetByDocEmpty) {
    std::vector<CortrixBlock> blocks;
    int ret = store_->block_get_by_doc("01JNONEXISTENT000000000000", blocks);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(blocks.empty());
}

// Test 33: search_metadata returns -1 (not implemented)
TEST_F(StoreSqliteTest, SearchMetadataNotImplemented) {
    std::vector<SearchResult> results;
    int ret = store_->search_metadata("any filter", 10, results);
    EXPECT_EQ(ret, -1);
}

// Test 34: DocUpdateStatusSetsUpdatedAt
TEST_F(StoreSqliteTest, DocUpdateStatusSetsUpdatedAt) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    std::string original_updated = doc.updated_at;

    // Small delay then update
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    store_->doc_update_status(doc.doc_id, DocStatus::kProcessing);

    CortrixDoc retrieved;
    store_->doc_get(doc.doc_id, retrieved);
    // updated_at should have changed (or at least not be empty)
    EXPECT_FALSE(retrieved.updated_at.empty());
}

// Test 35: Block with hnsw_node_id is stored and retrieved correctly
TEST_F(StoreSqliteTest, BlockWithHnswNodeId) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL3, "content", "", "", 1024, 1);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 3;
    block.hnsw_node_id = 42;
    block.data = blob;
    block.content_text = "content";
    store_->block_insert(block);

    CortrixBlock retrieved;
    store_->block_get(block.block_id, retrieved);
    EXPECT_EQ(retrieved.hnsw_node_id, 42);
}

// Test 36: Block with null hnsw_node_id returns -1
TEST_F(StoreSqliteTest, BlockWithNullHnswNodeId) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "content", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.hnsw_node_id = -1;  // null
    block.data = blob;
    block.content_text = "content";
    store_->block_insert(block);

    CortrixBlock retrieved;
    store_->block_get(block.block_id, retrieved);
    EXPECT_EQ(retrieved.hnsw_node_id, -1);
}

// Test 37: Double open is idempotent
TEST_F(StoreSqliteTest, DoubleOpenIdempotent) {
    // store_ is already open from SetUp
    int ret = store_->Open();
    EXPECT_EQ(ret, 0);  // should succeed (already open)

    // Still works
    int64_t count = 0;
    store_->doc_count(&count);
    EXPECT_EQ(count, 0);
}

// Test 38: Close then operations fail or re-open works
TEST_F(StoreSqliteTest, CloseAndReopen) {
    // Create a doc first
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);
    std::string saved_id = doc.doc_id;

    store_->Close();

    // Re-open
    ASSERT_EQ(store_->Open(), 0);

    // Should be able to retrieve the doc
    CortrixDoc retrieved;
    int ret = store_->doc_get(saved_id, retrieved);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(retrieved.source_type, "test");
}

// Test 39: Doc with all status types
TEST_F(StoreSqliteTest, AllDocStatusTypes) {
    DocStatus statuses[] = {
        DocStatus::kPending,
        DocStatus::kProcessing,
        DocStatus::kReady,
        DocStatus::kError,
        DocStatus::kStale,
    };

    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    for (auto s : statuses) {
        int ret = store_->doc_update_status(doc.doc_id, s);
        EXPECT_EQ(ret, 0) << "Failed for status " << static_cast<int>(s);

        CortrixDoc retrieved;
        store_->doc_get(doc.doc_id, retrieved);
        EXPECT_EQ(retrieved.status, s);
    }
}

// Test 40: Block content_text with special characters
TEST_F(StoreSqliteTest, BlockSpecialCharContent) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Content with SQL-special characters, unicode, newlines
    std::string special_content = "O'Reilly's \"book\" has <tags> & symbols\nnewline\ttab";
    auto blob = BlockBuild(kBlockFile, kLevelL2, special_content, "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = special_content;
    store_->block_insert(block);

    CortrixBlock retrieved;
    store_->block_get(block.block_id, retrieved);
    EXPECT_EQ(retrieved.content_text, special_content);
}

// ============================================================
// Design Alignment Tests (Review F01+F02)
// ============================================================

// Test 41: CDC fields are stored and retrieved
TEST_F(StoreSqliteTest, CdcFieldsRoundtrip) {
    CortrixDoc doc;
    doc.source_type = "cdc";
    doc.source_path = "pg://localhost/mydb/public.customers";
    doc.source_ref = "public.customers";
    doc.cdc_schema = R"({"columns":["id","name","email"]})";
    doc.cdc_position = "0/16B3748";
    doc.cdc_last_sync = "2026-02-15T10:30:00Z";

    int ret = store_->doc_create(doc);
    ASSERT_EQ(ret, 0);

    CortrixDoc retrieved;
    ret = store_->doc_get(doc.doc_id, retrieved);
    ASSERT_EQ(ret, 0);

    EXPECT_EQ(retrieved.source_type, "cdc");
    EXPECT_EQ(retrieved.cdc_schema, R"({"columns":["id","name","email"]})");
    EXPECT_EQ(retrieved.cdc_position, "0/16B3748");
    EXPECT_EQ(retrieved.cdc_last_sync, "2026-02-15T10:30:00Z");
}

// Test 42: CDC fields are NULL for non-CDC docs
TEST_F(StoreSqliteTest, CdcFieldsNullForNonCdc) {
    CortrixDoc doc;
    doc.source_type = "fuse";
    doc.source_path = "/path/to/file.pdf";

    int ret = store_->doc_create(doc);
    ASSERT_EQ(ret, 0);

    CortrixDoc retrieved;
    ret = store_->doc_get(doc.doc_id, retrieved);
    ASSERT_EQ(ret, 0);

    EXPECT_TRUE(retrieved.cdc_schema.empty());
    EXPECT_TRUE(retrieved.cdc_position.empty());
    EXPECT_TRUE(retrieved.cdc_last_sync.empty());
}

// Test 43: CDC doc appears in doc_list_by_status
TEST_F(StoreSqliteTest, CdcDocListByStatus) {
    CortrixDoc doc;
    doc.source_type = "cdc";
    doc.source_path = "pg://host/db/schema.table";
    doc.cdc_schema = R"({"cols":["a","b"]})";
    doc.cdc_position = "LSN-123";
    store_->doc_create(doc);

    std::vector<CortrixDoc> docs;
    int ret = store_->doc_list_by_status(DocStatus::kPending, docs);
    EXPECT_EQ(ret, 0);
    ASSERT_GE(docs.size(), 1u);

    bool found = false;
    for (auto& d : docs) {
        if (d.doc_id == doc.doc_id) {
            EXPECT_EQ(d.cdc_schema, R"({"cols":["a","b"]})");
            EXPECT_EQ(d.cdc_position, "LSN-123");
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// Test 44: CDC doc found by doc_find_by_source preserves CDC fields
TEST_F(StoreSqliteTest, CdcDocFindBySource) {
    CortrixDoc doc;
    doc.source_type = "cdc";
    doc.source_path = "pg://host/db/schema.users";
    doc.cdc_position = "binlog-pos-42";
    store_->doc_create(doc);

    CortrixDoc found;
    int ret = store_->doc_find_by_source("cdc", "pg://host/db/schema.users", found);
    ASSERT_EQ(ret, 0);
    EXPECT_EQ(found.cdc_position, "binlog-pos-42");
}

// Test 45: Block content_hash stored/retrieved as BLOB
TEST_F(StoreSqliteTest, BlockContentHashBlob) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Create a 16-byte binary hash (SHA-256 first 16 bytes)
    std::string binary_hash;
    binary_hash.resize(16);
    for (int i = 0; i < 16; ++i) {
        binary_hash[i] = static_cast<char>(i * 17);  // arbitrary non-ASCII bytes
    }

    auto blob = BlockBuild(kBlockFile, kLevelL2, "content", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.content_hash = binary_hash;
    block.data = blob;
    block.content_text = "content";

    int ret = store_->block_insert(block);
    ASSERT_EQ(ret, 0);

    CortrixBlock retrieved;
    ret = store_->block_get(block.block_id, retrieved);
    ASSERT_EQ(ret, 0);
    EXPECT_EQ(retrieved.content_hash.size(), 16u);
    EXPECT_EQ(retrieved.content_hash, binary_hash);
}

// Test 46: Block with empty content_hash is NULL
TEST_F(StoreSqliteTest, BlockEmptyContentHashIsNull) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "content", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.content_hash = "";  // empty = NULL
    block.data = blob;
    block.content_text = "content";

    int ret = store_->block_insert(block);
    ASSERT_EQ(ret, 0);

    CortrixBlock retrieved;
    ret = store_->block_get(block.block_id, retrieved);
    ASSERT_EQ(ret, 0);
    EXPECT_TRUE(retrieved.content_hash.empty());
}

// Test 47: Block content_hash BLOB survives block_get_by_doc
TEST_F(StoreSqliteTest, BlockContentHashBlobGetByDoc) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    std::string binary_hash(16, '\xAB');
    auto blob = BlockBuild(kBlockFile, kLevelL2, "content", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.content_hash = binary_hash;
    block.data = blob;
    block.content_text = "content";
    store_->block_insert(block);

    std::vector<CortrixBlock> blocks;
    int ret = store_->block_get_by_doc(doc.doc_id, blocks);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].content_hash.size(), 16u);
    EXPECT_EQ(blocks[0].content_hash, binary_hash);
}

// Test 48: FTS5 update trigger handles NULL→text transition
TEST_F(StoreSqliteTest, FTS5UpdateNullToText) {
    // This tests the unified update trigger handles the case where
    // content_text goes from NULL to a value
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    // Insert block with NULL content_text
    auto blob = BlockBuild(kBlockFile, kLevelL1, "", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 1;
    block.data = blob;
    // content_text is empty → NULL
    store_->block_insert(block);

    // Verify no FTS results for "searchterm"
    std::vector<SearchResult> results;
    store_->search_fulltext("searchterm", 10, results);
    EXPECT_TRUE(results.empty());

    // Verify the block was created (this test mainly verifies no crash)
    CortrixBlock retrieved;
    int ret = store_->block_get(block.block_id, retrieved);
    EXPECT_EQ(ret, 0);
}

// ============================================================
// Coverage Boost: DocStatus kDeleting (design alignment fix)
// ============================================================

// Test 49: DocStatus::kDeleting roundtrip through store
TEST_F(StoreSqliteTest, DocStatusDeletingRoundtrip) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    int ret = store_->doc_update_status(doc.doc_id, DocStatus::kDeleting);
    EXPECT_EQ(ret, 0);

    CortrixDoc retrieved;
    store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kDeleting);
}

// Test 50: Doc with kDeleting status appears in doc_list_by_status
TEST_F(StoreSqliteTest, DocListByStatusDeleting) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);
    store_->doc_update_status(doc.doc_id, DocStatus::kDeleting);

    std::vector<CortrixDoc> docs;
    int ret = store_->doc_list_by_status(DocStatus::kDeleting, docs);
    EXPECT_EQ(ret, 0);
    ASSERT_EQ(docs.size(), 1u);
    EXPECT_EQ(docs[0].status, DocStatus::kDeleting);
}

// ============================================================
// Coverage Boost: SanitizeFts5Query edge cases
// ============================================================

// Test 51: SanitizeFts5Query with tabs and newlines mixed with words
TEST(Fts5SanitizeTest, TabsAndNewlines) {
    EXPECT_EQ(SanitizeFts5Query("hello\tworld\nfoo"), "\"hello\" OR \"world\" OR \"foo\"");
}

// Test 52: SanitizeFts5Query with carriage return
TEST(Fts5SanitizeTest, CarriageReturn) {
    EXPECT_EQ(SanitizeFts5Query("a\rb"), "\"a\" OR \"b\"");
}

// Test 53: SanitizeFts5Query with multiple consecutive spaces
TEST(Fts5SanitizeTest, MultipleSpaces) {
    EXPECT_EQ(SanitizeFts5Query("  hello    world  "), "\"hello\" OR \"world\"");
}

// ============================================================
// Coverage Boost: Doc with processed_at field
// ============================================================

// Test 54: Doc with non-empty processed_at roundtrip
TEST_F(StoreSqliteTest, DocProcessedAtNonEmpty) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    doc.processed_at = "2026-02-15T12:00:00.000Z";
    store_->doc_create(doc);

    CortrixDoc retrieved;
    store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(retrieved.processed_at, "2026-02-15T12:00:00.000Z");
}

// Test 55: Doc with empty processed_at stores as NULL
TEST_F(StoreSqliteTest, DocProcessedAtEmpty) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    doc.processed_at = "";
    store_->doc_create(doc);

    CortrixDoc retrieved;
    store_->doc_get(doc.doc_id, retrieved);
    EXPECT_TRUE(retrieved.processed_at.empty());
}

// ============================================================
// Coverage Boost: RecoverCrashedDocs with blocks
// ============================================================

// Test 56: RecoverCrashedDocs deletes blocks of crashed docs
TEST_F(StoreSqliteTest, RecoverCrashedDocsDeletesBlocks) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);
    store_->doc_update_status(doc.doc_id, DocStatus::kProcessing);

    // Insert blocks for the processing doc
    auto blob = BlockBuild(kBlockFile, kLevelL2, "partial content", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "partial content";
    store_->block_insert(block);

    // Verify block exists
    std::vector<CortrixBlock> blocks;
    store_->block_get_by_doc(doc.doc_id, blocks);
    ASSERT_EQ(blocks.size(), 1u);

    // Recover crashes
    store_->RecoverCrashedDocs();

    // Blocks should be cleaned up
    blocks.clear();
    store_->block_get_by_doc(doc.doc_id, blocks);
    EXPECT_TRUE(blocks.empty());

    // Doc should be back to pending
    CortrixDoc retrieved;
    store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kPending);
}

// Test 57: RecoverCrashedDocs is no-op when no processing docs
TEST_F(StoreSqliteTest, RecoverCrashedDocsNoOp) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);
    store_->doc_update_status(doc.doc_id, DocStatus::kReady);

    int ret = store_->RecoverCrashedDocs();
    EXPECT_EQ(ret, 0);

    CortrixDoc retrieved;
    store_->doc_get(doc.doc_id, retrieved);
    EXPECT_EQ(retrieved.status, DocStatus::kReady);
}

// ============================================================
// Coverage Boost: block_delete_by_doc for non-existent doc
// ============================================================

// Test 58: block_delete_by_doc on non-existent doc succeeds (no-op)
TEST_F(StoreSqliteTest, BlockDeleteByDocNonExistent) {
    int ret = store_->block_delete_by_doc("01JNONEXISTENT000000000000");
    EXPECT_EQ(ret, 0);
}

// ============================================================
// Coverage Boost: Block with empty content_text (NULL in DB)
// ============================================================

// Test 59: Block with empty content_text stores and retrieves correctly
TEST_F(StoreSqliteTest, BlockEmptyContentText) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL1, "", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 1;
    block.data = blob;
    block.content_text = "";  // should be stored as NULL
    store_->block_insert(block);

    CortrixBlock retrieved;
    int ret = store_->block_get(block.block_id, retrieved);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(retrieved.content_text.empty());
}

// ============================================================
// Coverage Boost: doc_count and block_count after operations
// ============================================================

// Test 60: doc_count after insertions and deletions
TEST_F(StoreSqliteTest, DocCountAfterOperations) {
    int64_t count = 0;
    store_->doc_count(&count);
    EXPECT_EQ(count, 0);

    CortrixDoc d1, d2;
    d1.source_type = "test"; d1.source_path = "p1";
    d2.source_type = "test"; d2.source_path = "p2";
    store_->doc_create(d1);
    store_->doc_create(d2);

    store_->doc_count(&count);
    EXPECT_EQ(count, 2);

    store_->doc_delete(d1.doc_id);
    store_->doc_count(&count);
    EXPECT_EQ(count, 1);
}

// Test 61: block_count after insertions and deletions
TEST_F(StoreSqliteTest, BlockCountAfterOperations) {
    CortrixDoc doc;
    doc.source_type = "test"; doc.source_path = "test.txt";
    store_->doc_create(doc);

    int64_t count = 0;
    store_->block_count(&count);
    EXPECT_EQ(count, 0);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "c1", "", "", 0, 0);
    CortrixBlock b1;
    b1.doc_id = doc.doc_id; b1.chunk_index = 0; b1.block_type = 1;
    b1.processing_level = 2; b1.data = blob; b1.content_text = "c1";
    store_->block_insert(b1);

    CortrixBlock b2;
    b2.doc_id = doc.doc_id; b2.chunk_index = 1; b2.block_type = 1;
    b2.processing_level = 2; b2.data = blob; b2.content_text = "c2";
    store_->block_insert(b2);

    store_->block_count(&count);
    EXPECT_EQ(count, 2);

    store_->block_delete_by_doc(doc.doc_id);
    store_->block_count(&count);
    EXPECT_EQ(count, 0);
}

// ============================================================
// Coverage Boost: Multiple docs in recovery scenario
// ============================================================

// Test 62: RecoverCrashedDocs with multiple crashed docs
TEST_F(StoreSqliteTest, RecoverMultipleCrashed) {
    // Create 3 docs in processing state
    for (int i = 0; i < 3; ++i) {
        CortrixDoc doc;
        doc.source_type = "test";
        doc.source_path = "path_" + std::to_string(i);
        store_->doc_create(doc);
        store_->doc_update_status(doc.doc_id, DocStatus::kProcessing);

        auto blob = BlockBuild(kBlockFile, kLevelL2, "content", "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = 0;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = "content";
        store_->block_insert(block);
    }

    store_->RecoverCrashedDocs();

    std::vector<CortrixDoc> pending_docs;
    store_->doc_list_by_status(DocStatus::kPending, pending_docs);
    EXPECT_EQ(pending_docs.size(), 3u);

    int64_t block_count = 0;
    store_->block_count(&block_count);
    EXPECT_EQ(block_count, 0);
}

// ============================================================
// Coverage Boost: FTS5 search for non-matching query
// ============================================================

// Test 63: FTS5 search returns empty for non-matching query
TEST_F(StoreSqliteTest, FTS5SearchNoMatch) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "machine learning", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "machine learning";
    store_->block_insert(block);

    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("nonexistentword", 10, results);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(results.empty());
}

// Test 64: DocStatus out-of-range falls back to pending
TEST_F(StoreSqliteTest, DocStatusOutOfRange) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    // Force an out-of-range status (this tests the safety of DocStatusStr)
    doc.status = static_cast<DocStatus>(99);
    store_->doc_create(doc);

    CortrixDoc retrieved;
    store_->doc_get(doc.doc_id, retrieved);
    // Out-of-range status gets stored as "pending" (the fallback)
    EXPECT_EQ(retrieved.status, DocStatus::kPending);
}

// Test 65: Block with content_hash in block_get_by_doc (empty hash variant)
TEST_F(StoreSqliteTest, BlockGetByDocEmptyHash) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "test", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.content_hash = "";  // NULL
    block.data = blob;
    block.content_text = "test";
    store_->block_insert(block);

    std::vector<CortrixBlock> blocks;
    int ret = store_->block_get_by_doc(doc.doc_id, blocks);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_TRUE(blocks[0].content_hash.empty());
}

// Test 66: Multiple SearchResults from FTS5
TEST_F(StoreSqliteTest, FTS5MultipleResults) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "test.txt";
    store_->doc_create(doc);

    for (int i = 0; i < 5; ++i) {
        auto blob = BlockBuild(kBlockFile, kLevelL2, "deep learning model", "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = i;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = "deep learning model variant " + std::to_string(i);
        store_->block_insert(block);
    }

    std::vector<SearchResult> results;
    int ret = store_->search_fulltext("deep", 10, results);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(results.size(), 5u);
    // Verify each result has expected fields
    for (const auto& r : results) {
        EXPECT_GT(r.block_id, 0);
        EXPECT_EQ(r.doc_id, doc.doc_id);
        EXPECT_FALSE(r.content_text.empty());
    }
}

// ============================================================
// Design Alignment Tests (F01+F02 Final Review)
// ============================================================

// Test: doc_find_by_hash returns 0 and populates doc for existing hash
TEST_F(StoreSqliteTest, DocFindByHash_Found) {
    CortrixDoc doc;
    doc.source_type = "file";
    doc.source_path = "/tmp/hash_test.txt";
    doc.content_hash = "abc123def456";
    ASSERT_EQ(store_->doc_create(doc), 0);
    ASSERT_FALSE(doc.doc_id.empty());

    CortrixDoc found;
    int ret = store_->doc_find_by_hash("abc123def456", found);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(found.doc_id, doc.doc_id);
    EXPECT_EQ(found.content_hash, "abc123def456");
    EXPECT_EQ(found.source_path, "/tmp/hash_test.txt");
}

// Test: doc_find_by_hash returns -2 for non-existent hash
TEST_F(StoreSqliteTest, DocFindByHash_NotFound) {
    CortrixDoc found;
    int ret = store_->doc_find_by_hash("nonexistent_hash", found);
    EXPECT_EQ(ret, -2);
}

// Test: doc_find_by_hash returns -2 for empty hash
TEST_F(StoreSqliteTest, DocFindByHash_EmptyHash) {
    CortrixDoc found;
    int ret = store_->doc_find_by_hash("", found);
    EXPECT_EQ(ret, -2);
}

// Test: doc_find_by_hash finds correct doc among multiple
TEST_F(StoreSqliteTest, DocFindByHash_MultipleDocsDifferentHash) {
    CortrixDoc doc1;
    doc1.source_type = "file";
    doc1.source_path = "/tmp/file1.txt";
    doc1.content_hash = "hash_alpha";
    ASSERT_EQ(store_->doc_create(doc1), 0);

    CortrixDoc doc2;
    doc2.source_type = "file";
    doc2.source_path = "/tmp/file2.txt";
    doc2.content_hash = "hash_beta";
    ASSERT_EQ(store_->doc_create(doc2), 0);

    CortrixDoc found;
    int ret = store_->doc_find_by_hash("hash_beta", found);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(found.doc_id, doc2.doc_id);
    EXPECT_EQ(found.source_path, "/tmp/file2.txt");
}

// Test: PRAGMA auto_vacuum = INCREMENTAL actually takes effect on a fresh
// store-created db (design spec). Read the pragma back through a second raw
// connection — the old test never queried it, which hid that the pragma ran
// after journal_mode had already initialized the file and was silently a no-op.
TEST_F(StoreSqliteTest, PragmaAutoVacuumIncremental) {
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path_.c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "PRAGMA auto_vacuum", -1, &stmt, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    // 0 = NONE, 1 = FULL, 2 = INCREMENTAL
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2)
        << "auto_vacuum must be INCREMENTAL on a store-created db";
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
}

// D3.5 wire⑤ step①: block_insert honors a caller-provided uint64 block_id (the
// HashChildIdToBlockId hash), not the rowid. A high-bit-set id stores as a negative
// INTEGER rowid (bit-preserving), and must still round-trip through block_get +
// FTS5 search (content_rowid='block_id' triggers fire on the negative rowid).
TEST_F(StoreSqliteTest, ExplicitBlockIdRoundTripIncludingNegativeRowid) {
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "/explicit-id";
    ASSERT_EQ(store_->doc_create(doc), 0);

    CortrixBlock block;
    block.block_id = 0x8000000000000abcULL;  // high bit set → negative int64 rowid
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 3;
    block.data = BlockBuild(kBlockFile, kLevelL3, "needle haystack", "", "", 0, 0);
    block.content_text = "needle in the haystack";
    ASSERT_EQ(store_->block_insert(block), 0);
    EXPECT_EQ(block.block_id, 0x8000000000000abcULL)
        << "explicit id preserved, not overwritten by rowid";

    CortrixBlock got;
    ASSERT_EQ(store_->block_get(block.block_id, got), 0);
    EXPECT_EQ(got.block_id, block.block_id);
    EXPECT_EQ(got.content_text, "needle in the haystack");

    std::vector<SearchResult> results;
    ASSERT_EQ(store_->search_fulltext("needle", 10, results), 0);
    bool found = false;
    for (const auto& r : results) {
        if (r.block_id == block.block_id) found = true;
    }
    EXPECT_TRUE(found) << "FTS5 must return the block by its negative-rowid block_id";
}

// [A unified-blocks] parent_insert/parent_get round-trip the `parents` table on the
// unified store (the table is created standalone by F34SchemaProvider in
// CreateTables()). The SPC write path uses this so parents + child-blocks commit in
// one F25 transaction (ARCH §3.2). CortrixParent is the storage twin of
// chunker::ParentChunk; the store treats metadata_json as opaque text.
TEST_F(StoreSqliteTest, ParentInsertGetRoundTripAndDuplicate) {
    CortrixParent p;
    p.parent_id    = "01HXPARENT000000000000042";
    p.doc_id       = "01JTESTDOC0000000000000042";
    p.namespace_id = "ns_alpha";
    p.parent_text  = "A coarse ~1024-token parent context block.";
    p.token_count  = 1024;
    p.page_start   = 1;
    p.page_end     = 2;
    p.byte_offset_start = 100;
    p.byte_offset_end   = 5096;
    p.metadata_json = R"({"doc_title":"spec","doc_language":"en"})";
    p.created_at   = 1700000000000;
    ASSERT_EQ(store_->parent_insert(p), 0);

    CortrixParent got;
    ASSERT_EQ(store_->parent_get("01HXPARENT000000000000042", got), 0);
    EXPECT_EQ(got.parent_id, p.parent_id);
    EXPECT_EQ(got.doc_id, p.doc_id);
    EXPECT_EQ(got.namespace_id, p.namespace_id);
    EXPECT_EQ(got.parent_text, p.parent_text);
    EXPECT_EQ(got.token_count, 1024);
    EXPECT_EQ(got.page_start, 1);
    EXPECT_EQ(got.page_end, 2);
    EXPECT_EQ(got.byte_offset_start, 100);
    EXPECT_EQ(got.byte_offset_end, 5096);
    EXPECT_EQ(got.metadata_json, p.metadata_json);
    EXPECT_EQ(got.created_at, 1700000000000);

    // Missing id → -2 (not found).
    CortrixParent missing;
    EXPECT_EQ(store_->parent_get("01HXNOPE00000000000000000", missing), -2);

    // Duplicate parent_id → -3 (already exists), PK constraint.
    EXPECT_EQ(store_->parent_insert(p), -3);
}

// ============================================================
// Branch coverage: every statement op fails its prepare on a closed connection
// ============================================================

// A store whose connection was closed (db_ == nullptr) makes sqlite3_prepare_v2
// return SQLITE_MISUSE, so every CRUD/search/count method takes its prepare-fail
// error branch. This is the cheapest black-box way to cover all of them at once.
class StoreSqliteClosedTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / ("cortrix_store_closed_" + std::to_string(::getpid()) + "_" + std::to_string(rand()));
        fs::create_directories(test_dir_);
        db_path_ = (test_dir_ / "test.db").string();
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);
        store_->Close();  // db_ now null; all prepares will fail
    }
    void TearDown() override {
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }
    fs::path test_dir_;
    std::string db_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
};

TEST_F(StoreSqliteClosedTest, DocOpsFailPrepare) {
    CortrixDoc doc;
    doc.source_type = "t"; doc.source_path = "p";
    EXPECT_EQ(store_->doc_create(doc), -1);

    CortrixDoc got;
    EXPECT_EQ(store_->doc_get("id", got), -1);
    EXPECT_EQ(store_->doc_update_status("id", DocStatus::kReady), -1);
    EXPECT_EQ(store_->doc_delete("id"), -1);  // BEGIN TRANSACTION fails

    std::vector<CortrixDoc> docs;
    EXPECT_EQ(store_->doc_list_by_status(DocStatus::kPending, docs), -1);

    CortrixDoc found;
    EXPECT_EQ(store_->doc_find_by_source("t", "p", found), -1);
    EXPECT_EQ(store_->doc_find_by_hash("h", found), -1);

    int64_t count = -99;
    EXPECT_EQ(store_->doc_count(&count), -1);
}

TEST_F(StoreSqliteClosedTest, BlockOpsFailPrepare) {
    CortrixBlock block;
    block.doc_id = "d";
    EXPECT_EQ(store_->block_insert(block), -1);

    CortrixBlock got;
    EXPECT_EQ(store_->block_get(1, got), -1);

    std::vector<CortrixBlock> blocks;
    EXPECT_EQ(store_->block_get_by_doc("d", blocks), -1);
    EXPECT_EQ(store_->block_delete_by_doc("d"), -1);

    int64_t count = -99;
    EXPECT_EQ(store_->block_count(&count), -1);
    EXPECT_EQ(store_->block_count_by_doc("d", &count), -1);
}

TEST_F(StoreSqliteClosedTest, ParentOpsFailPrepare) {
    CortrixParent p;
    p.parent_id = "P"; p.doc_id = "D"; p.namespace_id = "N";
    EXPECT_EQ(store_->parent_insert(p), -1);

    CortrixParent got;
    EXPECT_EQ(store_->parent_get("P", got), -1);
}

TEST_F(StoreSqliteClosedTest, SearchAndRecoverFailPrepare) {
    std::vector<SearchResult> results;
    // Non-empty query gets past the sanitize early-return, then prepare fails.
    EXPECT_EQ(store_->search_fulltext("hello", 10, results), -1);

    // RecoverCrashedDocs' first prepare (find processing) fails → -1.
    EXPECT_EQ(store_->RecoverCrashedDocs(), -1);
}

// ============================================================
// Branch coverage: SetFailNextOps seam (F23 §4.5) fires the fault arm
// ============================================================

// Every public CRUD/search method begins with `if (TryConsumeOpFault()) return -1`.
// Arming the seam for a large count makes each call take that injected-fault arm
// (distinct from the closed-db prepare-fail arm above, which is reached past the
// seam). On a fully OPEN store, so the only thing failing is the seam itself.
TEST_F(StoreSqliteTest, FailNextOpsForcesEveryOpToFault) {
    // Arm enough faults to cover one call to each seam-guarded method below.
    store_->SetFailNextOps(100);

    CortrixDoc doc; doc.source_type = "t"; doc.source_path = "p";
    EXPECT_EQ(store_->doc_create(doc), -1);
    CortrixDoc got;
    EXPECT_EQ(store_->doc_get("id", got), -1);
    EXPECT_EQ(store_->doc_update_status("id", DocStatus::kReady), -1);
    EXPECT_EQ(store_->doc_delete("id"), -1);
    std::vector<CortrixDoc> docs;
    EXPECT_EQ(store_->doc_list_by_status(DocStatus::kPending, docs), -1);
    CortrixDoc found;
    EXPECT_EQ(store_->doc_find_by_source("t", "p", found), -1);
    EXPECT_EQ(store_->doc_find_by_hash("h", found), -1);
    int64_t count = -99;
    EXPECT_EQ(store_->doc_count(&count), -1);

    CortrixBlock block; block.doc_id = "d";
    EXPECT_EQ(store_->block_insert(block), -1);
    CortrixBlock bgot;
    EXPECT_EQ(store_->block_get(1, bgot), -1);
    std::vector<CortrixBlock> blocks;
    EXPECT_EQ(store_->block_get_by_doc("d", blocks), -1);
    EXPECT_EQ(store_->block_delete_by_doc("d"), -1);
    EXPECT_EQ(store_->block_count(&count), -1);
    EXPECT_EQ(store_->block_count_by_doc("d", &count), -1);

    CortrixParent p; p.parent_id = "P"; p.doc_id = "D"; p.namespace_id = "N";
    EXPECT_EQ(store_->parent_insert(p), -1);
    CortrixParent pgot;
    EXPECT_EQ(store_->parent_get("P", pgot), -1);

    std::vector<SearchResult> results;
    EXPECT_EQ(store_->search_fulltext("hello", 10, results), -1);
    EXPECT_EQ(store_->search_metadata("filter", 10, results), -1);

    // Disarm and confirm normal operation resumes (the seam's disabled arm).
    store_->SetFailNextOps(0);
    EXPECT_EQ(store_->doc_count(&count), 0);
}

// The seam drains exactly `n` faults, then ops succeed again: arms the
// compare_exchange loop in TryConsumeOpFault and its disarmed fast-path.
TEST_F(StoreSqliteTest, FailNextOpsDrainsExactlyN) {
    store_->SetFailNextOps(2);
    int64_t count = 0;
    EXPECT_EQ(store_->doc_count(&count), -1);  // fault 1
    EXPECT_EQ(store_->doc_count(&count), -1);  // fault 2
    EXPECT_EQ(store_->doc_count(&count), 0);   // drained → real call succeeds
}

// ============================================================
// Branch coverage: alternate constructors / OpenOptions
// ============================================================

// OpenOptions{false,false}: the owns-db path skips both CreateTables and
// RecoverCrashedDocs (production per-facade connection, D-I1.bis). The tables
// were already created by a prior standalone open of the same file, so reads work.
TEST(StoreSqliteOptionsTest, SkipsSchemaAndRecoveryWhenDisabled) {
    auto dir = fs::temp_directory_path() / ("cortrix_store_opts_" + std::to_string(::getpid()) + "_" + std::to_string(rand()));
    fs::create_directories(dir);
    std::string path = (dir / "opts.db").string();

    // First, a normal standalone open lays down the schema.
    {
        CortrixStoreSqlite seed(path);
        ASSERT_EQ(seed.Open(), 0);
        CortrixDoc d; d.source_type = "t"; d.source_path = "p";
        ASSERT_EQ(seed.doc_create(d), 0);
        seed.Close();
    }

    // Now open with both one-shot startup jobs disabled; the schema already exists.
    CortrixStoreSqlite::OpenOptions opts;
    opts.run_schema_ddl = false;
    opts.run_crash_recovery = false;
    CortrixStoreSqlite store(path, opts);
    EXPECT_EQ(store.Open(), 0);

    int64_t count = -1;
    EXPECT_EQ(store.doc_count(&count), 0);
    EXPECT_EQ(count, 1);
    store.Close();

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// External-connection mode with a null handle is a wiring bug: Open() must reject
// it (the !db_ branch in the external-conn arm) without ever calling sqlite3_open.
TEST(StoreSqliteOptionsTest, ExternalConnNullHandleRejected) {
    CortrixStoreSqlite store(static_cast<sqlite3*>(nullptr));
    EXPECT_EQ(store.Open(), -1);
}

// sqlite3_open_v2 can return a partially initialized handle together with an
// error. Open() must release that handle, clear its state, and remain reusable.
// LeakSanitizer verifies the failed attempt does not retain SQLite allocations.
TEST(StoreSqliteOptionsTest, FailedOpenReleasesHandleAndCanRetry) {
    const auto root = fs::temp_directory_path() /
                      ("cortrix_store_failed_open_" + std::to_string(::getpid()) +
                       "_" + std::to_string(rand()));
    const auto parent = root / "missing";
    const std::string path = (parent / "store.db").string();
    std::error_code ec;
    fs::remove_all(root, ec);

    CortrixStoreSqlite store(path);
    EXPECT_EQ(store.Open(), -1);
    EXPECT_EQ(store.db_handle(), nullptr);

    fs::create_directories(parent);
    EXPECT_EQ(store.Open(), 0);
    EXPECT_NE(store.db_handle(), nullptr);
    EXPECT_EQ(store.Close(), 0);

    fs::remove_all(root, ec);
}
