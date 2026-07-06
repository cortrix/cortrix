// enrich_state sidecar table: schema provider migration + store helpers
// (addendum §3.7 — coverage SoT for the chunk-level enrichment chain).
#include <gtest/gtest.h>

#include <sqlite3.h>

#include "cortrix/spc_enricher/enrich_state_schema_provider.h"
#include "cortrix/spc_enricher/enrich_state_store.h"

namespace cortrix::spc {
namespace {

class EnrichStateStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        EnrichStateSchemaProvider provider;
        ASSERT_TRUE(provider.Migrate(db_, 0, provider.CurrentVersion()).ok());
    }
    void TearDown() override { sqlite3_close(db_); }

    static EnrichStateRow MakeRow(uint64_t block_id, const std::string& doc_id,
                                  const std::string& status,
                                  int64_t next_retry_at = 0) {
        EnrichStateRow r;
        r.block_id = block_id;
        r.doc_id = doc_id;
        r.child_id = "child-" + std::to_string(block_id);
        r.status = status;
        r.next_retry_at = next_retry_at;
        r.updated_at = 1000;
        return r;
    }

    sqlite3* db_ = nullptr;
};

TEST_F(EnrichStateStoreTest, MigrateIsIdempotentAndRejectsUnknownVersions) {
    EnrichStateSchemaProvider provider;
    // Second run over an existing table is a no-op success.
    EXPECT_TRUE(provider.Migrate(db_, 0, 1).ok());
    // from == to is tolerated (migrator skip contract).
    EXPECT_TRUE(provider.Migrate(db_, 1, 1).ok());
    // Any other pair is a version mismatch.
    EXPECT_FALSE(provider.Migrate(db_, 1, 2).ok());
    EXPECT_FALSE(provider.Migrate(nullptr, 0, 1).ok());
}

TEST_F(EnrichStateStoreTest, UpsertInsertsThenOverwrites) {
    EnrichStateRow r = MakeRow(11, "doc-a", kEnrichStatusPendingRetry, 500);
    r.failed_members = "f03,f38";
    r.last_error = "CX_ERR_ENRICHER_LLM_API: transport";
    ASSERT_TRUE(UpsertEnrichState(db_, r).ok());

    r.status = kEnrichStatusOk;
    r.failed_members.clear();
    r.last_error.clear();
    r.attempts = 3;
    r.next_retry_at = 0;  // ok rows carry NULL
    r.updated_at = 2000;
    ASSERT_TRUE(UpsertEnrichState(db_, r).ok());

    auto rows = ListEnrichStateForDoc(db_, "doc-a", "");
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    const EnrichStateRow& got = rows.value()[0];
    EXPECT_EQ(got.block_id, 11u);
    EXPECT_EQ(got.child_id, "child-11");
    EXPECT_EQ(got.status, kEnrichStatusOk);
    EXPECT_TRUE(got.failed_members.empty());
    EXPECT_TRUE(got.last_error.empty());
    EXPECT_EQ(got.attempts, 3);
    EXPECT_EQ(got.next_retry_at, 0);
    EXPECT_EQ(got.updated_at, 2000);
}

TEST_F(EnrichStateStoreTest, UpsertRejectsEmptyStatusAndNullDb) {
    EXPECT_FALSE(UpsertEnrichState(db_, MakeRow(1, "d", "")).ok());
    EXPECT_FALSE(UpsertEnrichState(nullptr, MakeRow(1, "d", kEnrichStatusOk)).ok());
}

TEST_F(EnrichStateStoreTest, ListForDocFiltersByStatusAndOrdersByBlockId) {
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(22, "doc-a", kEnrichStatusPendingRetry, 500)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(21, "doc-a", kEnrichStatusOk)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(23, "doc-b", kEnrichStatusPendingRetry, 500)).ok());

    auto all = ListEnrichStateForDoc(db_, "doc-a", "");
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all.value().size(), 2u);
    EXPECT_EQ(all.value()[0].block_id, 21u);  // ordered by block_id
    EXPECT_EQ(all.value()[1].block_id, 22u);

    auto pending = ListEnrichStateForDoc(db_, "doc-a", kEnrichStatusPendingRetry);
    ASSERT_TRUE(pending.ok());
    ASSERT_EQ(pending.value().size(), 1u);
    EXPECT_EQ(pending.value()[0].block_id, 22u);
}

TEST_F(EnrichStateStoreTest, ListDueDocsGatesOnStatusAndDueTimeOrdersOldestFirst) {
    // doc-a due at 100 (oldest), doc-b due at 200, doc-c not yet due, doc-d ok.
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(1, "doc-b", kEnrichStatusPendingRetry, 200)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(2, "doc-a", kEnrichStatusPendingRetry, 100)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(3, "doc-a", kEnrichStatusPendingRetry, 900)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(4, "doc-c", kEnrichStatusPendingRetry, 5000)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(5, "doc-d", kEnrichStatusOk)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(6, "doc-e", kEnrichStatusFailedPermanent, 1)).ok());

    auto due = ListDueDocs(db_, /*now_unix=*/300, /*limit=*/10);
    ASSERT_TRUE(due.ok());
    ASSERT_EQ(due.value().size(), 2u);
    EXPECT_EQ(due.value()[0], "doc-a");  // MIN(next_retry_at)=100 < doc-b 200
    EXPECT_EQ(due.value()[1], "doc-b");

    auto limited = ListDueDocs(db_, 300, 1);
    ASSERT_TRUE(limited.ok());
    ASSERT_EQ(limited.value().size(), 1u);
    EXPECT_EQ(limited.value()[0], "doc-a");

    auto none = ListDueDocs(db_, 300, 0);
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());
}

TEST_F(EnrichStateStoreTest, LeaseDocRetriesPushesOnlyPendingRowsOfThatDoc) {
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(1, "doc-a", kEnrichStatusPendingRetry, 100)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(2, "doc-a", kEnrichStatusOk)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(3, "doc-b", kEnrichStatusPendingRetry, 100)).ok());

    ASSERT_TRUE(LeaseDocRetries(db_, "doc-a", /*lease_until=*/9999, /*now_unix=*/500).ok());

    // doc-a no longer due at now=300; doc-b still due.
    auto due = ListDueDocs(db_, 300, 10);
    ASSERT_TRUE(due.ok());
    ASSERT_EQ(due.value().size(), 1u);
    EXPECT_EQ(due.value()[0], "doc-b");

    // The ok row was untouched (next_retry_at stays NULL, updated_at stays 1000).
    auto rows = ListEnrichStateForDoc(db_, "doc-a", kEnrichStatusOk);
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(rows.value()[0].next_retry_at, 0);
    EXPECT_EQ(rows.value()[0].updated_at, 1000);
}

TEST_F(EnrichStateStoreTest, CountEnrichStatesGroupsByStatus) {
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(1, "doc-a", kEnrichStatusOk)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(2, "doc-a", kEnrichStatusOk)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(3, "doc-a", kEnrichStatusPendingRetry, 5)).ok());
    ASSERT_TRUE(UpsertEnrichState(db_, MakeRow(4, "doc-b", kEnrichStatusFailedPermanent)).ok());

    auto counts = CountEnrichStates(db_);
    ASSERT_TRUE(counts.ok());
    EXPECT_EQ(counts.value().total, 4);
    EXPECT_EQ(counts.value().ok, 2);
    EXPECT_EQ(counts.value().pending_retry, 1);
    EXPECT_EQ(counts.value().failed_permanent, 1);
}

TEST_F(EnrichStateStoreTest, HelpersFailCleanlyWithoutTable) {
    sqlite3* bare = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &bare), SQLITE_OK);
    EXPECT_FALSE(UpsertEnrichState(bare, MakeRow(1, "d", kEnrichStatusOk)).ok());
    EXPECT_FALSE(ListEnrichStateForDoc(bare, "d", "").ok());
    EXPECT_FALSE(ListDueDocs(bare, 1, 1).ok());
    EXPECT_FALSE(LeaseDocRetries(bare, "d", 1, 1).ok());
    EXPECT_FALSE(CountEnrichStates(bare).ok());
    sqlite3_close(bare);
}

}  // namespace
}  // namespace cortrix::spc
