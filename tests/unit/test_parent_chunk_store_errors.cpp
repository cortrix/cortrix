#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/chunker/types.h"
#include "cortrix/store/sqlite_parent_chunk_store.h"
#include "cortrix/store/store_errors.h"

// Error-path coverage for SqliteParentChunkStore (§ 2.5.1 CX_ERR_STORE_DB_ERROR).
// Standalone DB-error injection without a real DB fault: an un-creatable path
// (Open fails), and writes against a closed store (every op returns DB_ERROR).
namespace cortrix::store {
namespace {

using cortrix::chunker::ChildChunk;
using cortrix::chunker::ParentChunk;

ParentChunk P(const std::string& id) {
    ParentChunk p;
    p.parent_id = id; p.doc_id = "D"; p.namespace_id = "N";
    p.parent_text = "t"; p.token_count = 1; p.created_at = 1;
    return p;
}
ChildChunk C(const std::string& id, const std::string& pid) {
    ChildChunk c;
    c.child_id = id; c.parent_id = pid; c.doc_id = "D"; c.namespace_id = "N";
    c.child_text = "t"; c.token_count = 1; c.created_at = 1;
    return c;
}

TEST(ParentChunkStoreErrorTest, OpenUncreatablePathFails) {
    // A path under a non-existent directory cannot be created → DB_ERROR.
    SqliteParentChunkStore store("/nonexistent_dir_xyz_42/parents.db");
    Status s = store.Open();
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find(store_errors::kDbError), std::string::npos);
}

TEST(ParentChunkStoreErrorTest, WritesOnClosedStoreReturnDbError) {
    SqliteParentChunkStore store(":memory:");  // never Open()ed
    Status s1 = store.PutParentWithChildren(P("P1"), {C("C1", "P1")});
    EXPECT_FALSE(s1.ok());
    EXPECT_NE(s1.message().find(store_errors::kDbError), std::string::npos);

    Status s2 = store.PutChunkerOutput({P("P1")}, {});
    EXPECT_FALSE(s2.ok());

    auto b = store.BulkGetParents({"P1"});
    EXPECT_FALSE(b.ok());
    EXPECT_NE(b.status().message().find(store_errors::kDbError), std::string::npos);
}

TEST(ParentChunkStoreErrorTest, OpsAfterCloseReturnDbError) {
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());
    ASSERT_TRUE(store.PutParentWithChildren(P("P1"), {}).ok());
    EXPECT_TRUE(store.GetParent("P1").ok());
    store.Close();
    // After close every path takes the "store not open" DB_ERROR branch.
    EXPECT_FALSE(store.GetParent("P1").ok());
    EXPECT_FALSE(store.BulkGetParents({"P1"}).ok());
    EXPECT_FALSE(store.PutChunkerOutput({P("P2")}, {}).ok());
}

TEST(ParentChunkStoreErrorTest, DuplicateParentMidBatchRollsBackWholeTransaction) {
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());
    ASSERT_TRUE(store.PutParentWithChildren(P("EXIST"), {}).ok());

    // Batch where the 2nd parent collides with the existing PK → parent-insert
    // failure path + full rollback (the 1st parent "NEW1" must NOT persist).
    Status s = store.PutChunkerOutput({P("NEW1"), P("EXIST")}, {});
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find(store_errors::kDbError), std::string::npos);
    EXPECT_FALSE(store.GetParent("NEW1").ok());  // rolled back
}

TEST(ParentChunkStoreErrorTest, DuplicateChildMidBatchRollsBack) {
    // The parents all insert cleanly; the 2nd child collides with the 1st on the
    // child_id PK → child-insert failure path + full rollback. Nothing persists.
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());

    std::vector<ParentChunk> parents = {P("PA")};
    std::vector<ChildChunk> children = {C("CDUP", "PA"), C("CDUP", "PA")};  // duplicate child_id
    Status s = store.PutChunkerOutput(parents, children);
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find(store_errors::kDbError), std::string::npos);

    // The parent from the same batch was rolled back too.
    EXPECT_FALSE(store.GetParent("PA").ok());
}

TEST(ParentChunkStoreErrorTest, GetParentMissingReturnsNotFound) {
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());
    auto r = store.GetParent("does-not-exist");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find(store_errors::kNotFound), std::string::npos);
}

TEST(ParentChunkStoreErrorTest, BulkGetParentsEmptyInputReturnsEmpty) {
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());
    auto r = store.BulkGetParents({});  // empty input → early-return empty vector
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST(ParentChunkStoreErrorTest, BulkGetParentsPartialHitsDropMisses) {
    // Result contains only the ids that exist (missing ids are silently dropped),
    // exercising both the SQLITE_ROW and SQLITE_DONE arms of the step switch.
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());
    ASSERT_TRUE(store.PutParentWithChildren(P("HIT1"), {}).ok());
    ASSERT_TRUE(store.PutParentWithChildren(P("HIT2"), {}).ok());

    auto r = store.BulkGetParents({"HIT1", "MISS", "HIT2"});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 2u);
    EXPECT_EQ(r.value()[0].parent_id, "HIT1");
    EXPECT_EQ(r.value()[1].parent_id, "HIT2");
}

TEST(ParentChunkStoreErrorTest, OpenIsIdempotent) {
    // A second Open() on an already-open store short-circuits to Ok (db_ != null).
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());
    EXPECT_TRUE(store.Open().ok());
    EXPECT_TRUE(store.PutParentWithChildren(P("X"), {}).ok());
}

TEST(ParentChunkStoreErrorTest, FlatChildWithEmptyParentIdBindsNull) {
    // A flat child (empty parent_id) takes the sqlite3_bind_null arm of
    // BindTextOrNull; the insert must still succeed and round-trip the parent.
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());
    ChildChunk flat = C("FLAT", /*parent_id=*/"");
    ASSERT_TRUE(store.PutChunkerOutput({P("PRoot")}, {flat}).ok());
    EXPECT_TRUE(store.GetParent("PRoot").ok());
}

TEST(ParentChunkStoreErrorTest, MetadataJsonInvalidParsesToDefault) {
    // metadata_json round-trips through MetaToJson on write; on read MetaFromJson
    // tolerates a malformed/empty blob and returns a default DocumentMetadata.
    // We write a parent whose metadata is all-default, so the read-back JSON parses
    // into the same defaults (exercises the parse + value() default arms).
    SqliteParentChunkStore store(":memory:");
    ASSERT_TRUE(store.Open().ok());
    ParentChunk p = P("PMeta");  // P() leaves DocumentMetadata default-constructed
    ASSERT_TRUE(store.PutParentWithChildren(p, {}).ok());

    auto got = store.GetParent("PMeta");
    ASSERT_TRUE(got.ok());
    EXPECT_TRUE(got.value().metadata.filename.empty());
    EXPECT_EQ(got.value().metadata.page_count, -1);  // MetaFromJson default for page_count
}

TEST(ParentChunkStoreErrorTest, ReopenAfterCloseWorks) {
    SqliteParentChunkStore store("/tmp/f34_reopen_test.db");
    ASSERT_TRUE(store.Open().ok());
    ASSERT_TRUE(store.PutParentWithChildren(P("RP"), {}).ok());
    store.Close();
    ASSERT_TRUE(store.Open().ok());  // schema migration is idempotent (IF NOT EXISTS)
    EXPECT_TRUE(store.GetParent("RP").ok());  // persisted across reopen
    store.Close();
    std::remove("/tmp/f34_reopen_test.db");
}

}  // namespace
}  // namespace cortrix::store
