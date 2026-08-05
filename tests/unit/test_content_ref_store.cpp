#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_error.h"
#include "cortrix/catalog/default_content_ref_store.h"

// S2.3 coverage: DefaultContentRefStore against a real in-memory catalog.db.
namespace cortrix::catalog {
namespace {

bool HasCxCode(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;
}

// Seed a file_locations row so the ref_count aggregate is tracked (this row is
// normally created by the write path, write coordinator).
void SeedFileLocation(sqlite3* db, const std::string& file_hash,
                      const std::string& status = "active",
                      std::optional<int64_t> deleted_at = std::nullopt) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO file_locations (file_hash, unit_id, ns_id, tenant_id, "
        "ref_count, first_seen_at, status, deleted_at, created_at) "
        "VALUES (?, 'u1', 'ns1', 't1', 0, 0, ?, ?, 0)";
    ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, file_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    if (deleted_at.has_value()) sqlite3_bind_int64(stmt, 3, *deleted_at);
    else sqlite3_bind_null(stmt, 3);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

class ContentRefStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        // file_locations.unit_id → units(unit_id) → tenants(tenant_id) are real
        // FKs (foreign_keys=ON), so seed the parents the helper rows reference.
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        store_ = std::make_unique<DefaultContentRefStore>(catalog_.db());
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultContentRefStore> store_;
};

TEST_F(ContentRefStoreTest, IncrementCreatesRefAndCountsWhenFileTracked) {
    SeedFileLocation(catalog_.db(), "fh1");
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());
    auto c = store_->GetRefCount("fh1");
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 1);
}

TEST_F(ContentRefStoreTest, IncrementIsIdempotentPerTriple) {
    SeedFileLocation(catalog_.db(), "fh1");
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());  // same triple
    auto c = store_->GetRefCount("fh1");
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 1) << "re-referencing the same (file,ns,doc) must not double count";
}

TEST_F(ContentRefStoreTest, DistinctTriplesEachCount) {
    SeedFileLocation(catalog_.db(), "fh1");
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns2", "doc2").ok());
    auto c = store_->GetRefCount("fh1");
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 2);
}

TEST_F(ContentRefStoreTest, DecrementReturnsRemainingCount) {
    SeedFileLocation(catalog_.db(), "fh1");
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns2", "doc2").ok());
    auto rem = store_->DecrementRef("fh1", "ns1", "doc1");
    ASSERT_TRUE(rem.ok()) << rem.status().message();
    EXPECT_EQ(rem.value(), 1);
}

TEST_F(ContentRefStoreTest, DecrementBelowZeroIsRefCountNegative) {
    SeedFileLocation(catalog_.db(), "fh1");  // ref_count starts at 0
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());   // → 1
    auto first = store_->DecrementRef("fh1", "ns1", "doc1");        // → 0
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), 0);
    // The (file,ns,doc) row is already 'deleted' now, so a second decrement of
    // the same triple is a no-op (removed=false) and count stays 0 (not an error).
    auto again = store_->DecrementRef("fh1", "ns1", "doc1");
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(), 0);
}

TEST_F(ContentRefStoreTest, DecrementWhenAggregateAlreadyZeroIsNegativeError) {
    SeedFileLocation(catalog_.db(), "fh1");
    // Force an inconsistent state: an active content_ref but file_locations
    // ref_count still 0 (e.g. aggregate not seeded by writer). Decrement must
    // refuse to go negative.
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "INSERT INTO content_refs (file_hash, ns_id, doc_id, target_unit_id, status, created_at) "
        "VALUES ('fh1','ns1','docX','', 'active', 0)", nullptr, nullptr, nullptr), SQLITE_OK);
    auto r = store_->DecrementRef("fh1", "ns1", "docX");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(HasCxCode(r.status(), "CX_ERR_REF_COUNT_NEGATIVE")) << r.status().message();
}

TEST_F(ContentRefStoreTest, GetRefCountUnknownFileIsZero) {
    auto c = store_->GetRefCount("never-seen");
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 0);
}

TEST_F(ContentRefStoreTest, IncrementWithoutFileLocationStillTracksRefRow) {
    // No file_locations row: the content_refs row is still recorded, GetRefCount
    // returns 0 (aggregate untracked). Tolerates writer-created-later ordering.
    ASSERT_TRUE(store_->IncrementRef("orphan", "ns1", "doc1").ok());
    auto c = store_->GetRefCount("orphan");
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 0);
}

TEST_F(ContentRefStoreTest, GCCandidatesAreZeroRefDeletedBeforeCutoff) {
    // fh_old: ref 0, deleted at t=100  → candidate when cutoff > 100
    // fh_new: ref 0, deleted at t=900  → not a candidate when cutoff = 500
    // fh_live: ref 1, active           → never a candidate
    SeedFileLocation(catalog_.db(), "fh_old", "deleted", 100);
    SeedFileLocation(catalog_.db(), "fh_new", "deleted", 900);
    SeedFileLocation(catalog_.db(), "fh_live", "active");
    ASSERT_TRUE(store_->IncrementRef("fh_live", "ns1", "d").ok());  // ref→1

    auto cand = store_->GetGCCandidates(500);
    ASSERT_TRUE(cand.ok());
    ASSERT_EQ(cand.value().size(), 1u);
    EXPECT_EQ(cand.value()[0], "fh_old");
}

// ── branch coverage: reactivation, prepare/step failures, count guards ───────

// Re-referencing a previously soft-deleted (file,ns,doc) triple takes the
// newly_added path again: the ON CONFLICT DO UPDATE reactivates the row to
// 'active' and the aggregate is bumped back up.
TEST_F(ContentRefStoreTest, ReReferenceAfterDeleteReactivatesAndCounts) {
    SeedFileLocation(catalog_.db(), "fh1");
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());     // count 1
    auto rem = store_->DecrementRef("fh1", "ns1", "doc1");           // count 0, soft-deleted
    ASSERT_TRUE(rem.ok());
    EXPECT_EQ(rem.value(), 0);
    // Same triple again → not already_active → newly_added → reactivate + ++count.
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());
    auto c = store_->GetRefCount("fh1");
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 1);
}

// Decrementing a triple that has no active content_refs row at all is a no-op
// (removed=false) → the aggregate is untouched and the call still succeeds.
TEST_F(ContentRefStoreTest, DecrementUnknownTripleIsNoOp) {
    SeedFileLocation(catalog_.db(), "fh1");
    ASSERT_TRUE(store_->IncrementRef("fh1", "ns1", "doc1").ok());  // count 1
    auto r = store_->DecrementRef("fh1", "ns1", "never-referenced");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 1) << "no active row for that doc → aggregate unchanged";
}

// removed=true but no file_locations row exists (current stays -1) → neither the
// current==0 guard nor the current>0 decrement fires; commit succeeds, count 0.
TEST_F(ContentRefStoreTest, DecrementWithoutAggregateRowSkipsDecrement) {
    // IncrementRef without a file_locations row still records the content_refs row.
    ASSERT_TRUE(store_->IncrementRef("orphan", "ns1", "doc1").ok());
    auto r = store_->DecrementRef("orphan", "ns1", "doc1");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 0);  // no aggregate tracked
}

// Dropping content_refs makes the IncrementRef probe prepare fail → kInternalError
// (and the BEGIN txn is rolled back).
TEST_F(ContentRefStoreTest, IncrementProbePrepareFailureIsInternalError) {
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE content_refs", nullptr, nullptr, nullptr),
              SQLITE_OK);
    Status s = store_->IncrementRef("fh1", "ns1", "doc1");
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(HasCxCode(s, "CX_ERR_INTERNAL_ERROR")) << s.message();
}

// Dropping content_refs makes the DecrementRef UPDATE prepare fail → kInternalError.
TEST_F(ContentRefStoreTest, DecrementPrepareFailureIsInternalError) {
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE content_refs", nullptr, nullptr, nullptr),
              SQLITE_OK);
    auto r = store_->DecrementRef("fh1", "ns1", "doc1");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(HasCxCode(r.status(), "CX_ERR_INTERNAL_ERROR")) << r.status().message();
}

// Dropping file_locations makes GetRefCount's prepare fail → kInternalError.
TEST_F(ContentRefStoreTest, GetRefCountPrepareFailureIsInternalError) {
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE file_locations", nullptr, nullptr, nullptr),
              SQLITE_OK);
    auto c = store_->GetRefCount("fh1");
    EXPECT_FALSE(c.ok());
    EXPECT_TRUE(HasCxCode(c.status(), "CX_ERR_INTERNAL_ERROR")) << c.status().message();
}

// Dropping file_locations makes GetGCCandidates' prepare fail → kInternalError.
TEST_F(ContentRefStoreTest, GCCandidatesPrepareFailureIsInternalError) {
    ASSERT_EQ(sqlite3_exec(catalog_.db(), "DROP TABLE file_locations", nullptr, nullptr, nullptr),
              SQLITE_OK);
    auto cand = store_->GetGCCandidates(500);
    EXPECT_FALSE(cand.ok());
    EXPECT_TRUE(HasCxCode(cand.status(), "CX_ERR_INTERNAL_ERROR")) << cand.status().message();
}

// GC candidate query excludes rows with a NULL deleted_at (the deleted_at IS NOT
// NULL clause) and rows deleted at-or-after the cutoff — covering the empty result.
TEST_F(ContentRefStoreTest, GCCandidatesEmptyWhenNothingQualifies) {
    SeedFileLocation(catalog_.db(), "fh_active");                 // active, never a candidate
    SeedFileLocation(catalog_.db(), "fh_no_ts", "deleted");       // deleted but deleted_at NULL
    SeedFileLocation(catalog_.db(), "fh_recent", "deleted", 900); // deleted after the cutoff
    auto cand = store_->GetGCCandidates(500);
    ASSERT_TRUE(cand.ok());
    EXPECT_TRUE(cand.value().empty());
}

}  // namespace
}  // namespace cortrix::catalog
