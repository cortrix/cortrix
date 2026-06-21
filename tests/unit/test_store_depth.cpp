// DEPTH anti-regression tests for src/store/ -- targets the OPEN-2 three-stage GC
// lifecycle (doc_soft_delete / doc_restore / doc_list_deleted_before), block_count_by_doc,
// parent FK + roundtrip edges, the SetFailNextOps fault seam on additional public methods,
// and CortrixBlobLocal del/load/store boundary semantics -- paths thinly or not covered by
// test_cortrix_store_sqlite.cpp / test_cortrix_blob_local.cpp.
//
// Deterministic: temp/in-memory SQLite + temp blob dir, no network/LLM.
// Suite/fixture names are globally unique (StoreDepth* prefix) per gtest's global-string
// suite-name rule.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/store/cortrix_blob_local.h"
#include "cortrix/store/cortrix_store_sqlite.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;

// --------------------------------------------------------------------------
// SQLite store: OPEN-2 soft-delete lifecycle + parent FK + count + fault seam
// --------------------------------------------------------------------------

class StoreDepthSqliteFx : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_store_depth_" + std::to_string(::testing::UnitTest::GetInstance()
                                                                 ->random_seed()) +
                     "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(test_dir_);
        db_path_ = (test_dir_ / "depth.db").string();
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);
    }
    void TearDown() override {
        if (store_) store_->Close();
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    // Create a 'ready' doc with a preset id (doc_create honors a preset doc_id).
    void MakeReadyDoc(const std::string& id) {
        CortrixDoc d;
        d.doc_id = id;
        d.source_type = "file";
        d.source_path = "/p/" + id;
        d.status = DocStatus::kReady;
        ASSERT_EQ(store_->doc_create(d), 0);
    }

    fs::path test_dir_;
    std::string db_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
};

TEST_F(StoreDepthSqliteFx, SoftDeleteMarksDeletedAndRestoreFlipsBackReady) {
    MakeReadyDoc("d1");
    EXPECT_EQ(store_->doc_soft_delete("d1", /*now_ms=*/1000), 0);
    CortrixDoc got;
    ASSERT_EQ(store_->doc_get("d1", got), 0);
    EXPECT_EQ(got.status, DocStatus::kDeleted);

    EXPECT_EQ(store_->doc_restore("d1"), 0);
    ASSERT_EQ(store_->doc_get("d1", got), 0);
    EXPECT_EQ(got.status, DocStatus::kReady);
}

TEST_F(StoreDepthSqliteFx, SoftDeleteMissingDocReturnsNotFound) {
    EXPECT_EQ(store_->doc_soft_delete("ghost", 1000), -2);
}

TEST_F(StoreDepthSqliteFx, RestoreOnLiveDocIsNotFound) {
    MakeReadyDoc("live");
    // Not soft-deleted -> the WHERE status='deleted' guard matches nothing -> -2.
    EXPECT_EQ(store_->doc_restore("live"), -2);
}

TEST_F(StoreDepthSqliteFx, RestoreMissingDocReturnsNotFound) {
    EXPECT_EQ(store_->doc_restore("nope"), -2);
}

TEST_F(StoreDepthSqliteFx, SoftDeleteIsIdempotentOnAlreadyDeleted) {
    MakeReadyDoc("d2");
    ASSERT_EQ(store_->doc_soft_delete("d2", 1000), 0);
    // Re-soft-deleting an already-deleted doc still matches the row (changes>0) -> 0.
    EXPECT_EQ(store_->doc_soft_delete("d2", 2000), 0);
}

TEST_F(StoreDepthSqliteFx, ListDeletedBeforeCutoffFiltersByDeletedAt) {
    MakeReadyDoc("old");
    MakeReadyDoc("recent");
    MakeReadyDoc("alive");
    ASSERT_EQ(store_->doc_soft_delete("old", 100), 0);
    ASSERT_EQ(store_->doc_soft_delete("recent", 5000), 0);
    // "alive" stays ready.

    std::vector<CortrixDoc> out;
    ASSERT_EQ(store_->doc_list_deleted_before(/*cutoff_ms=*/1000, out), 0);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].doc_id, "old");
}

TEST_F(StoreDepthSqliteFx, ListDeletedBeforeFutureCutoffCatchesAll) {
    MakeReadyDoc("x");
    MakeReadyDoc("y");
    ASSERT_EQ(store_->doc_soft_delete("x", 100), 0);
    ASSERT_EQ(store_->doc_soft_delete("y", 200), 0);
    std::vector<CortrixDoc> out;
    ASSERT_EQ(store_->doc_list_deleted_before(/*cutoff_ms=*/99999999, out), 0);
    EXPECT_EQ(out.size(), 2u);
}

TEST_F(StoreDepthSqliteFx, ListDeletedBeforeEmptyWhenNoneDeleted) {
    MakeReadyDoc("z");
    std::vector<CortrixDoc> out;
    ASSERT_EQ(store_->doc_list_deleted_before(99999999, out), 0);
    EXPECT_TRUE(out.empty());
}

TEST_F(StoreDepthSqliteFx, BlockCountByDocCountsOnlyThatDoc) {
    MakeReadyDoc("docA");
    MakeReadyDoc("docB");
    for (int i = 0; i < 3; ++i) {
        CortrixBlock b;
        b.doc_id = "docA";
        b.chunk_index = i;
        b.data = {1, 2, 3};  // NOT NULL payload
        b.content_text = "a" + std::to_string(i);
        ASSERT_EQ(store_->block_insert(b), 0);
    }
    CortrixBlock b;
    b.doc_id = "docB";
    b.data = {9};
    b.content_text = "b0";
    ASSERT_EQ(store_->block_insert(b), 0);

    int64_t cnt = -1;
    ASSERT_EQ(store_->block_count_by_doc("docA", &cnt), 0);
    EXPECT_EQ(cnt, 3);
    ASSERT_EQ(store_->block_count_by_doc("docB", &cnt), 0);
    EXPECT_EQ(cnt, 1);
}

TEST_F(StoreDepthSqliteFx, BlockCountByDocZeroForUnknownDoc) {
    int64_t cnt = -1;
    ASSERT_EQ(store_->block_count_by_doc("nope", &cnt), 0);
    EXPECT_EQ(cnt, 0);
}

TEST_F(StoreDepthSqliteFx, ParentInsertGetRoundTripAllFields) {
    MakeReadyDoc("pdoc");
    CortrixParent p;
    p.parent_id = "par1";
    p.doc_id = "pdoc";
    p.namespace_id = "nsP";
    p.parent_text = "the full parent span";
    p.token_count = 42;
    p.page_start = 1;
    p.page_end = 3;
    p.byte_offset_start = 10;
    p.byte_offset_end = 99;
    p.metadata_json = R"({"k":"v"})";
    p.created_at = 1717000000000;
    ASSERT_EQ(store_->parent_insert(p), 0);

    CortrixParent got;
    ASSERT_EQ(store_->parent_get("par1", got), 0);
    EXPECT_EQ(got.parent_id, "par1");
    EXPECT_EQ(got.doc_id, "pdoc");
    EXPECT_EQ(got.namespace_id, "nsP");
    EXPECT_EQ(got.parent_text, "the full parent span");
    EXPECT_EQ(got.token_count, 42);
    EXPECT_EQ(got.page_start, 1);
    EXPECT_EQ(got.page_end, 3);
    EXPECT_EQ(got.byte_offset_start, 10);
    EXPECT_EQ(got.byte_offset_end, 99);
    EXPECT_EQ(got.metadata_json, R"({"k":"v"})");
    EXPECT_EQ(got.created_at, 1717000000000);
}

TEST_F(StoreDepthSqliteFx, ParentGetMissingReturnsNotFound) {
    CortrixParent got;
    EXPECT_EQ(store_->parent_get("absent", got), -2);
}

TEST_F(StoreDepthSqliteFx, ChildBlockColumnsRoundTrip) {
    MakeReadyDoc("cdoc");
    CortrixParent p;
    p.parent_id = "par2";
    p.doc_id = "cdoc";
    p.namespace_id = "nsC";
    p.parent_text = "span";
    ASSERT_EQ(store_->parent_insert(p), 0);

    CortrixBlock child;
    child.doc_id = "cdoc";
    child.chunk_index = 0;
    child.data = {7, 7};
    child.content_text = "child text";
    child.child_id = "child-ulid";
    child.parent_id = "par2";
    child.token_count = 12;
    child.parent_offset = 5;
    child.metadata_json = R"({"child":true})";
    ASSERT_EQ(store_->block_insert(child), 0);

    std::vector<CortrixBlock> blocks;
    ASSERT_EQ(store_->block_get_by_doc("cdoc", blocks), 0);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].child_id, "child-ulid");
    EXPECT_EQ(blocks[0].parent_id, "par2");
    EXPECT_EQ(blocks[0].token_count, 12);
    EXPECT_EQ(blocks[0].parent_offset, 5);
}

TEST_F(StoreDepthSqliteFx, FailNextOpsAlsoFaultsSoftDeleteAndParent) {
    MakeReadyDoc("ff");
    // Arm two faults: the next two public CRUD ops fail with -1 without touching db.
    store_->SetFailNextOps(2);
    EXPECT_EQ(store_->doc_soft_delete("ff", 1), -1);
    CortrixParent got;
    EXPECT_EQ(store_->parent_get("ff", got), -1);
    // Third call is past the armed window -> real behavior resumes.
    CortrixDoc d;
    EXPECT_EQ(store_->doc_get("ff", d), 0);
}

TEST_F(StoreDepthSqliteFx, FailNextOpsZeroDisablesSeam) {
    MakeReadyDoc("noop");
    store_->SetFailNextOps(0);
    CortrixDoc d;
    EXPECT_EQ(store_->doc_get("noop", d), 0);
}

TEST_F(StoreDepthSqliteFx, ParentInsertDuplicateReturnsAlreadyExists) {
    MakeReadyDoc("pd");
    CortrixParent p;
    p.parent_id = "dup";
    p.doc_id = "pd";
    p.namespace_id = "ns";
    p.parent_text = "t";
    ASSERT_EQ(store_->parent_insert(p), 0);
    // Re-inserting the same parent_id (PK) -> -3 already exists.
    EXPECT_EQ(store_->parent_insert(p), -3);
}

TEST_F(StoreDepthSqliteFx, NonChildBlockHasNullSentinelFieldsOnReadBack) {
    MakeReadyDoc("nd");
    CortrixBlock b;
    b.doc_id = "nd";
    b.chunk_index = 0;
    b.data = {5};
    b.content_text = "plain";
    // child_id/parent_id empty, token_count/parent_offset left at -1 sentinel -> NULL.
    ASSERT_EQ(store_->block_insert(b), 0);
    std::vector<CortrixBlock> out;
    ASSERT_EQ(store_->block_get_by_doc("nd", out), 0);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0].child_id.empty());
    EXPECT_TRUE(out[0].parent_id.empty());
    EXPECT_EQ(out[0].token_count, -1);
    EXPECT_EQ(out[0].parent_offset, -1);
}

TEST_F(StoreDepthSqliteFx, BlockInsertAssignsRowidWhenIdZero) {
    MakeReadyDoc("rid");
    CortrixBlock b;
    b.doc_id = "rid";
    b.chunk_index = 0;
    b.data = {1, 2};
    b.content_text = "x";
    ASSERT_EQ(b.block_id, 0u);
    ASSERT_EQ(store_->block_insert(b), 0);
    EXPECT_NE(b.block_id, 0u);  // rowid backfilled

    CortrixBlock got;
    ASSERT_EQ(store_->block_get(b.block_id, got), 0);
    EXPECT_EQ(got.doc_id, "rid");
    EXPECT_EQ(got.content_text, "x");
}

TEST_F(StoreDepthSqliteFx, DocCountIncludesSoftDeletedRows) {
    MakeReadyDoc("a");
    MakeReadyDoc("b");
    ASSERT_EQ(store_->doc_soft_delete("a", 1000), 0);
    int64_t cnt = -1;
    ASSERT_EQ(store_->doc_count(&cnt), 0);
    // Soft delete only flips status; the row is still present until Stage-2 GC.
    EXPECT_EQ(cnt, 2);
}

TEST_F(StoreDepthSqliteFx, DocListByStatusFindsDeletedAfterSoftDelete) {
    MakeReadyDoc("sd");
    ASSERT_EQ(store_->doc_soft_delete("sd", 1000), 0);
    std::vector<CortrixDoc> ready;
    ASSERT_EQ(store_->doc_list_by_status(DocStatus::kReady, ready), 0);
    EXPECT_TRUE(ready.empty());
    std::vector<CortrixDoc> deleted;
    ASSERT_EQ(store_->doc_list_by_status(DocStatus::kDeleted, deleted), 0);
    ASSERT_EQ(deleted.size(), 1u);
    EXPECT_EQ(deleted[0].doc_id, "sd");
}

TEST_F(StoreDepthSqliteFx, BlockLargeBinaryDataRoundTrips) {
    MakeReadyDoc("big");
    CortrixBlock b;
    b.doc_id = "big";
    b.chunk_index = 0;
    b.data.assign(64 * 1024, 0xAB);  // 64 KiB payload
    b.content_text = "large";
    ASSERT_EQ(store_->block_insert(b), 0);
    CortrixBlock got;
    ASSERT_EQ(store_->block_get(b.block_id, got), 0);
    EXPECT_EQ(got.data.size(), 64u * 1024u);
    EXPECT_EQ(got.data.front(), 0xAB);
    EXPECT_EQ(got.data.back(), 0xAB);
}

// --------------------------------------------------------------------------
// CortrixBlobLocal: del/load/store boundary + return-code semantics
// --------------------------------------------------------------------------

class StoreDepthBlobFx : public ::testing::Test {
protected:
    void SetUp() override {
        base_dir_ = fs::temp_directory_path() /
                    ("cortrix_blob_depth_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(base_dir_);
        blob_ = std::make_unique<CortrixBlobLocal>(base_dir_.string());
    }
    void TearDown() override {
        blob_.reset();
        std::error_code ec;
        fs::remove_all(base_dir_, ec);
    }
    fs::path base_dir_;
    std::unique_ptr<CortrixBlobLocal> blob_;
};

TEST_F(StoreDepthBlobFx, LoadMissingReturnsNotFound) {
    std::vector<uint8_t> out;
    EXPECT_EQ(blob_->load("ns", "missing", out), -2);
}

TEST_F(StoreDepthBlobFx, DelMissingReturnsNotFound) {
    EXPECT_EQ(blob_->del("ns", "missing"), -2);
}

TEST_F(StoreDepthBlobFx, StoreThenDelThenLoadIsNotFound) {
    std::string payload = "hello blob";
    ASSERT_EQ(blob_->store("ns", "k1", payload.data(), payload.size()), 0);
    std::vector<uint8_t> out;
    ASSERT_EQ(blob_->load("ns", "k1", out), 0);
    EXPECT_EQ(std::string(out.begin(), out.end()), payload);

    ASSERT_EQ(blob_->del("ns", "k1"), 0);
    EXPECT_EQ(blob_->load("ns", "k1", out), -2);
}

TEST_F(StoreDepthBlobFx, OverwriteReplacesContent) {
    std::string a = "AAAA";
    std::string b = "BB";
    ASSERT_EQ(blob_->store("ns", "k", a.data(), a.size()), 0);
    ASSERT_EQ(blob_->store("ns", "k", b.data(), b.size()), 0);
    std::vector<uint8_t> out;
    ASSERT_EQ(blob_->load("ns", "k", out), 0);
    EXPECT_EQ(std::string(out.begin(), out.end()), "BB");
}

TEST_F(StoreDepthBlobFx, NamespaceIsolationSameDocId) {
    std::string a = "from-a";
    std::string b = "from-b";
    ASSERT_EQ(blob_->store("nsA", "same", a.data(), a.size()), 0);
    ASSERT_EQ(blob_->store("nsB", "same", b.data(), b.size()), 0);
    std::vector<uint8_t> out;
    ASSERT_EQ(blob_->load("nsA", "same", out), 0);
    EXPECT_EQ(std::string(out.begin(), out.end()), "from-a");
    ASSERT_EQ(blob_->load("nsB", "same", out), 0);
    EXPECT_EQ(std::string(out.begin(), out.end()), "from-b");
}

TEST_F(StoreDepthBlobFx, EmptyPayloadRoundTrips) {
    ASSERT_EQ(blob_->store("ns", "empty", "", 0), 0);
    std::vector<uint8_t> out{1, 2, 3};  // pre-fill to confirm it is cleared
    ASSERT_EQ(blob_->load("ns", "empty", out), 0);
    EXPECT_TRUE(out.empty());
}

TEST_F(StoreDepthBlobFx, BinaryWithEmbeddedNullsPreserved) {
    std::vector<uint8_t> data = {0x00, 0xFF, 0x00, 0x10, 0x00};
    ASSERT_EQ(blob_->store("ns", "bin", data.data(), data.size()), 0);
    std::vector<uint8_t> out;
    ASSERT_EQ(blob_->load("ns", "bin", out), 0);
    EXPECT_EQ(out, data);
}

TEST_F(StoreDepthBlobFx, DelTwiceSecondIsNotFound) {
    std::string p = "x";
    ASSERT_EQ(blob_->store("ns", "dd", p.data(), p.size()), 0);
    EXPECT_EQ(blob_->del("ns", "dd"), 0);
    EXPECT_EQ(blob_->del("ns", "dd"), -2);
}

}  // namespace
}  // namespace cortrix
