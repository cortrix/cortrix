// The operator's exit for ledger rows that gave up (ResetGivenUpEnrichRows) and
// the audit's settled count. Kept in its own translation unit: both call
// functions that the earlier store helpers did not have, so the behavioural tests
// that compile against the earlier code (test_enrich_state_store.cpp,
// test_enrich_routes.cpp) can be built and run on their own.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include "cortrix/spc_enricher/enrich_state_schema_provider.h"
#include "cortrix/spc_enricher/enrich_state_store.h"

namespace cortrix::spc {
namespace {

class EnrichResetTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        EnrichStateSchemaProvider provider;
        ASSERT_TRUE(provider.Migrate(db_, 0, provider.CurrentVersion()).ok());
        // The blocks shape the audit and the reset read.
        ASSERT_EQ(sqlite3_exec(db_,
            "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, doc_id TEXT,"
            " child_id TEXT, block_type INTEGER DEFAULT 1, enriched_score REAL,"
            " contextualized_status SMALLINT DEFAULT 0,"
            " contextualized_embedding BLOB DEFAULT NULL, metadata_json TEXT)",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }
    void TearDown() override { sqlite3_close(db_); }

    void SeedChild(uint64_t block_id, bool score, int ctx_status) {
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db_,
            "INSERT INTO blocks(block_id, doc_id, child_id, block_type,"
            " enriched_score, contextualized_status) VALUES(?1,'doc-a',?2,1,?3,?4)",
            -1, &st, nullptr), SQLITE_OK);
        const std::string child = "child-" + std::to_string(block_id);
        sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(block_id));
        sqlite3_bind_text(st, 2, child.c_str(), -1, SQLITE_TRANSIENT);
        if (score) sqlite3_bind_double(st, 3, 0.5); else sqlite3_bind_null(st, 3);
        sqlite3_bind_int(st, 4, ctx_status);
        ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
        sqlite3_finalize(st);
    }

    void SeedRow(uint64_t block_id, const char* status, int attempts,
                 const std::string& failed_members, int64_t next_retry_at) {
        EnrichStateRow r;
        r.block_id = block_id;
        r.doc_id = "doc-a";
        r.child_id = "child-" + std::to_string(block_id);
        r.status = status;
        r.attempts = attempts;
        r.failed_members = failed_members;
        r.last_error = "provider timeout";
        r.next_retry_at = next_retry_at;
        r.updated_at = 1000;
        ASSERT_TRUE(UpsertEnrichState(db_, r).ok());
    }

    EnrichStateRow RowOf(uint64_t block_id) {
        auto rows = ListEnrichStateForDoc(db_, "doc-a", "");
        if (rows.ok()) {
            for (const auto& r : rows.value()) if (r.block_id == block_id) return r;
        }
        return EnrichStateRow{};
    }

    sqlite3* db_ = nullptr;
};

// Every given-up row whose chunk still exists goes back to pending with a fresh
// count, due now; a given-up row whose chunk is gone is left terminal and
// counted; rows in the other two states are not touched.
TEST_F(EnrichResetTest, MovesLiveRowsAndSkipsOrphans) {
    SeedChild(71, /*score=*/false, /*ctx=*/0);
    SeedRow(71, kEnrichStatusFailedPermanent, 8, "f03,f35,f38", 0);
    SeedRow(72, kEnrichStatusFailedPermanent, 8, "f38", 0);  // no blocks row: chunk gone
    SeedChild(73, false, 0);
    SeedRow(73, kEnrichStatusPendingRetry, 3, "f03", 4000);
    SeedChild(74, true, 1);
    SeedRow(74, kEnrichStatusOk, 1, "", 0);

    auto r = ResetGivenUpEnrichRows(db_, /*now=*/900);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().moved, 1);
    EXPECT_EQ(r.value().orphaned, 1);

    const EnrichStateRow live = RowOf(71);
    EXPECT_EQ(live.status, kEnrichStatusPendingRetry);
    EXPECT_EQ(live.attempts, 0);                   // a fresh run of attempts
    EXPECT_EQ(live.next_retry_at, 900);            // due now
    EXPECT_EQ(live.last_error, "reset by operator");
    EXPECT_EQ(live.failed_members, "f03,f35,f38");  // the debt itself is kept
    EXPECT_EQ(live.updated_at, 900);

    const EnrichStateRow orphan = RowOf(72);
    EXPECT_EQ(orphan.status, kEnrichStatusFailedPermanent);  // left terminal
    EXPECT_EQ(orphan.attempts, 8);
    EXPECT_EQ(orphan.next_retry_at, 0);
    EXPECT_EQ(orphan.last_error, "provider timeout");

    const EnrichStateRow pending = RowOf(73);
    EXPECT_EQ(pending.status, kEnrichStatusPendingRetry);
    EXPECT_EQ(pending.attempts, 3);                // not reset
    EXPECT_EQ(pending.next_retry_at, 4000);        // backoff kept

    const EnrichStateRow ok = RowOf(74);
    EXPECT_EQ(ok.status, kEnrichStatusOk);
    EXPECT_EQ(ok.attempts, 1);

    // A second reset has nothing live left to move; the orphan is still reported.
    auto r2 = ResetGivenUpEnrichRows(db_, 950);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value().moved, 0);
    EXPECT_EQ(r2.value().orphaned, 1);
}

TEST_F(EnrichResetTest, NothingToResetIsZeroes) {
    SeedChild(81, false, 0);
    SeedRow(81, kEnrichStatusPendingRetry, 2, "f03", 4000);
    auto r = ResetGivenUpEnrichRows(db_, 900);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().moved, 0);
    EXPECT_EQ(r.value().orphaned, 0);
    EXPECT_EQ(RowOf(81).attempts, 2);
}

TEST_F(EnrichResetTest, FailsCleanlyWithoutTable) {
    sqlite3* bare = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &bare), SQLITE_OK);
    EXPECT_FALSE(ResetGivenUpEnrichRows(bare, 1).ok());
    EXPECT_FALSE(ResetGivenUpEnrichRows(nullptr, 1).ok());
    sqlite3_close(bare);
}

// The audit reports how many terminal rows it settled, and only those: a terminal
// row that still owes something and an in-flight row for a complete chunk are
// not counted.
TEST_F(EnrichResetTest, AuditReportsSettledCount) {
    SeedChild(91, /*score=*/true, /*ctx=*/1);  // complete for a chain of f03,f35
    SeedRow(91, kEnrichStatusFailedPermanent, 8, "f35", 0);
    SeedChild(92, true, 1);
    SeedRow(92, kEnrichStatusFailedPermanent, 8, "f38", 0);  // gave up on an unconfigured member
    SeedChild(93, false, 0);
    SeedRow(93, kEnrichStatusFailedPermanent, 8, "f03,f35", 0);  // still owes: stays
    SeedChild(94, true, 1);
    SeedRow(94, kEnrichStatusPendingRetry, 2, "f35", 900);  // in flight: the worker's

    int settled = -1;
    auto n = SynthesizeEnrichAuditRows(db_, {"f03", "f35"}, /*now=*/500, &settled);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0);
    EXPECT_EQ(settled, 2);
    EXPECT_EQ(RowOf(91).status, kEnrichStatusOk);
    EXPECT_EQ(RowOf(92).status, kEnrichStatusOk);
    EXPECT_EQ(RowOf(93).status, kEnrichStatusFailedPermanent);
    EXPECT_EQ(RowOf(94).status, kEnrichStatusPendingRetry);

    // Nothing left to settle: the count starts from zero on every call.
    settled = -1;
    auto n2 = SynthesizeEnrichAuditRows(db_, {"f03", "f35"}, 600, &settled);
    ASSERT_TRUE(n2.ok());
    EXPECT_EQ(settled, 0);
}

}  // namespace
}  // namespace cortrix::spc
