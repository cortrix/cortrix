#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/chunker/types.h"
#include "cortrix/store/sqlite_parent_chunk_store.h"
#include "cortrix/store/store_errors.h"

namespace cortrix::store {
namespace {

using cortrix::chunker::ChildChunk;
using cortrix::chunker::ParentChunk;

ParentChunk MakeParent(const std::string& pid, const std::string& doc = "DOC1") {
    ParentChunk p;
    p.parent_id = pid;
    p.doc_id = doc;
    p.namespace_id = "NS1";
    p.parent_text = "parent text for " + pid;
    p.token_count = 42;
    p.page_start = 1;
    p.page_end = 2;
    p.byte_offset_start = 100;
    p.byte_offset_end = 200;
    p.metadata.doc_title = "Title";
    p.metadata.doc_language = "zh";
    p.metadata.mime_type = "application/pdf";
    p.created_at = 1700000000000;
    return p;
}

ChildChunk MakeChild(const std::string& cid, const std::string& pid,
                     uint32_t idx, const std::string& doc = "DOC1") {
    ChildChunk c;
    c.child_id = cid;
    c.parent_id = pid;
    c.doc_id = doc;
    c.namespace_id = "NS1";
    c.child_text = "child text " + cid;
    c.token_count = 10;
    c.parent_offset = idx * 10;
    c.chunk_index = idx;
    c.created_at = 1700000000000;
    return c;
}

class ParentChunkStoreTest : public ::testing::Test {
protected:
    SqliteParentChunkStore store{":memory:"};
    void SetUp() override { ASSERT_TRUE(store.Open().ok()); }
};

TEST_F(ParentChunkStoreTest, OpenIsIdempotent) {
    EXPECT_TRUE(store.Open().ok());  // second open is a no-op
}

TEST_F(ParentChunkStoreTest, PutAndGetRoundTrips) {
    ParentChunk p = MakeParent("P1");
    std::vector<ChildChunk> kids = {MakeChild("C1", "P1", 0), MakeChild("C2", "P1", 1)};
    p.child_ids = {"C1", "C2"};
    ASSERT_TRUE(store.PutParentWithChildren(p, kids).ok());

    auto r = store.GetParent("P1");
    ASSERT_TRUE(r.ok()) << r.status().message();
    const ParentChunk& got = r.value();
    EXPECT_EQ(got.parent_id, "P1");
    EXPECT_EQ(got.doc_id, "DOC1");
    EXPECT_EQ(got.namespace_id, "NS1");
    EXPECT_EQ(got.parent_text, "parent text for P1");
    EXPECT_EQ(got.token_count, 42u);
    EXPECT_EQ(got.page_start, 1u);
    EXPECT_EQ(got.page_end, 2u);
    EXPECT_EQ(got.byte_offset_start, 100u);
    EXPECT_EQ(got.byte_offset_end, 200u);
    // metadata_json round-trips with F06 ParseDocMeta field names.
    EXPECT_EQ(got.metadata.doc_title, "Title");
    EXPECT_EQ(got.metadata.doc_language, "zh");
    EXPECT_EQ(got.metadata.mime_type, "application/pdf");
    EXPECT_EQ(got.created_at, 1700000000000);
}

TEST_F(ParentChunkStoreTest, GetMissingReturnsNotFound) {
    auto r = store.GetParent("nope");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
    EXPECT_NE(r.status().message().find(store_errors::kNotFound), std::string::npos);
}

TEST_F(ParentChunkStoreTest, BulkGetReturnsHitsInInputOrderDroppingMisses) {
    ASSERT_TRUE(store.PutParentWithChildren(MakeParent("P1"), {}).ok());
    ASSERT_TRUE(store.PutParentWithChildren(MakeParent("P2"), {}).ok());
    ASSERT_TRUE(store.PutParentWithChildren(MakeParent("P3"), {}).ok());

    auto r = store.BulkGetParents({"P3", "MISSING", "P1"});
    ASSERT_TRUE(r.ok()) << r.status().message();
    ASSERT_EQ(r.value().size(), 2u);  // MISSING dropped
    EXPECT_EQ(r.value()[0].parent_id, "P3");  // input order preserved
    EXPECT_EQ(r.value()[1].parent_id, "P1");
}

TEST_F(ParentChunkStoreTest, BulkGetEmptyInputReturnsEmpty) {
    auto r = store.BulkGetParents({});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(ParentChunkStoreTest, PutChunkerOutputPersistsAllParents) {
    std::vector<ParentChunk> parents = {MakeParent("PA"), MakeParent("PB")};
    std::vector<ChildChunk> children = {
        MakeChild("CA1", "PA", 0), MakeChild("CB1", "PB", 1)};
    ASSERT_TRUE(store.PutChunkerOutput(parents, children).ok());
    EXPECT_TRUE(store.GetParent("PA").ok());
    EXPECT_TRUE(store.GetParent("PB").ok());
}

TEST_F(ParentChunkStoreTest, FlatChildrenWithEmptyParentIdInsert) {
    // Flat-fallback children have an empty parent_id (NULL FK). They must insert
    // without a parents row.
    std::vector<ChildChunk> flat = {MakeChild("F1", "", 0), MakeChild("F2", "", 1)};
    Status s = store.PutChunkerOutput({}, flat);
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(ParentChunkStoreTest, DuplicateParentIdFailsTransactionally) {
    ASSERT_TRUE(store.PutParentWithChildren(MakeParent("DUP"), {}).ok());
    // Re-inserting the same PK violates PRIMARY KEY → DB_ERROR, rolled back.
    Status s = store.PutParentWithChildren(MakeParent("DUP"), {});
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find(store_errors::kDbError), std::string::npos);
}

TEST_F(ParentChunkStoreTest, UnopenedStoreReturnsDbError) {
    SqliteParentChunkStore closed{":memory:"};  // not Open()ed
    auto r = closed.GetParent("x");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find(store_errors::kDbError), std::string::npos);
}

}  // namespace
}  // namespace cortrix::store
