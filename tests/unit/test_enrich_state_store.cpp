// enrich_state sidecar table: schema provider migration + store helpers
// (addendum — coverage SoT for the chunk-level enrichment chain).
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
    r.failed_members = "enrich,hype";
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

class EnrichAuditTest : public EnrichStateStoreTest {
protected:
    void SetUp() override {
        EnrichStateStoreTest::SetUp();
        // Minimal blocks shape the audit reads (framework + enricher/contextual retrieval columns).
        ASSERT_EQ(sqlite3_exec(db_,
            "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, doc_id TEXT,"
            " child_id TEXT, block_type INTEGER DEFAULT 1, enriched_score REAL,"
            " contextualized_status SMALLINT DEFAULT 0, metadata_json TEXT)",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }

    void SeedChild(uint64_t block_id, const std::string& child, bool score,
                   int ctx_status) {
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db_,
            "INSERT INTO blocks(block_id, doc_id, child_id, block_type,"
            " enriched_score, contextualized_status) VALUES(?1,'doc-a',?2,1,?3,?4)",
            -1, &st, nullptr), SQLITE_OK);
        sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(block_id));
        sqlite3_bind_text(st, 2, child.c_str(), -1, SQLITE_TRANSIENT);
        if (score) sqlite3_bind_double(st, 3, 0.5); else sqlite3_bind_null(st, 3);
        sqlite3_bind_int(st, 4, ctx_status);
        ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
        sqlite3_finalize(st);
    }

    void SeedHypeFor(const std::string& source_child) {
        // Explicit high block_id so auto-rowid never collides with seeded children.
        const std::string sql =
            "INSERT INTO blocks(block_id, doc_id, child_id, block_type, metadata_json)"
            " VALUES(" + std::to_string(9000 + next_hype_id_++) +
            ",'doc-a','',16,'{\"source_child_id\":\"" + source_child + "\"}')";
        ASSERT_EQ(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }

    int next_hype_id_ = 0;
};

// Audit synthesizes pending rows exactly for the missing configured artifacts;
// covered chunks and already-pending rows are untouched.
TEST_F(EnrichAuditTest, SynthesizesOwedMembersOnly) {
    SeedChild(1, "child-full", /*score=*/true, /*ctx=*/1);   // fully covered
    SeedHypeFor("child-full");
    SeedChild(2, "child-bare", false, 0);                    // owes all three
    SeedChild(3, "child-partial", true, 2);                  // owes contextual,hype (ctx failed)
    SeedChild(4, "child-tracked", false, 0);                 // already pending (attempts=3)
    EnrichStateRow tracked = MakeRow(4, "doc-a", kEnrichStatusPendingRetry, 99);
    tracked.attempts = 3;
    tracked.failed_members = "enrich";
    ASSERT_TRUE(UpsertEnrichState(db_, tracked).ok());

    auto n = SynthesizeEnrichAuditRows(db_, {"enrich", "contextual", "hype"}, /*now=*/500);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);  // child-bare + child-partial

    auto rows = ListEnrichStateForDoc(db_, "doc-a", "");
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 3u);  // 2 synthesized + 1 pre-tracked
    for (const auto& r : rows.value()) {
        if (r.block_id == 2) {
            EXPECT_EQ(r.failed_members, "enrich,contextual,hype");
            EXPECT_EQ(r.next_retry_at, 500);
        } else if (r.block_id == 3) {
            EXPECT_EQ(r.failed_members, "contextual,hype");
        } else if (r.block_id == 4) {
            EXPECT_EQ(r.attempts, 3);           // untouched
            EXPECT_EQ(r.next_retry_at, 99);     // backoff kept
        }
    }
}

// P4: a failed_permanent row (terminal give-up at the 8x/48h ceiling) must NOT be
// resurrected by audit. Its artifact is still missing by definition, so a naive audit
// would re-synthesize it and INSERT OR REPLACE would reset attempts=0 → infinite retry.
TEST_F(EnrichAuditTest, DoesNotResurrectFailedPermanent) {
    SeedChild(5, "child-doomed", /*score=*/false, /*ctx=*/0);  // bare: artifacts missing
    EnrichStateRow gaveup = MakeRow(5, "doc-a", kEnrichStatusFailedPermanent, 0);
    gaveup.attempts = 8;                    // hit the ceiling
    gaveup.failed_members = "hype";
    ASSERT_TRUE(UpsertEnrichState(db_, gaveup).ok());

    auto n = SynthesizeEnrichAuditRows(db_, {"enrich", "contextual", "hype"}, /*now=*/500);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0);  // nothing synthesized — the terminal row is skipped

    auto rows = ListEnrichStateForDoc(db_, "doc-a", "");
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(rows.value()[0].status, kEnrichStatusFailedPermanent);  // stayed terminal
    EXPECT_EQ(rows.value()[0].attempts, 8);                            // NOT reset to 0
}

// P7: contextual "done" requires the ANN label row too — columns-only data (pre-V2 store /
// partial index loss) must be re-owed, or the contextualized path never gets votes.
// When contextual_vec_labels is absent entirely (isolated pre-V2 store) the audit
// keeps the legacy ctx_status-only semantics (covered by the other tests above,
// whose fixture has no label table).
TEST_F(EnrichAuditTest, OwesContextualWhenLabelMissingDespiteColumns) {
    ASSERT_EQ(sqlite3_exec(db_,
        "CREATE TABLE contextual_vec_labels (label INTEGER PRIMARY KEY,"
        " block_id INTEGER NOT NULL, child_id TEXT NOT NULL)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    // Both children have full columns + hype; only child-21 has its label indexed.
    SeedChild(21, "child-labeled", /*score=*/true, /*ctx=*/1);
    SeedHypeFor("child-labeled");
    SeedChild(22, "child-lostlabel", /*score=*/true, /*ctx=*/1);
    SeedHypeFor("child-lostlabel");
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO contextual_vec_labels(label, block_id, child_id)"
        " VALUES(7001, 21, 'child-labeled')",
        nullptr, nullptr, nullptr), SQLITE_OK);

    auto n = SynthesizeEnrichAuditRows(db_, {"enrich", "contextual", "hype"}, /*now=*/500);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 1);  // only the label-less child is re-owed

    auto rows = ListEnrichStateForDoc(db_, "doc-a", kEnrichStatusPendingRetry);
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(rows.value()[0].block_id, 22u);
    EXPECT_EQ(rows.value()[0].failed_members, "contextual");
}

// Members outside the configured chain are never owed (hype unconfigured here).
TEST_F(EnrichAuditTest, RespectsConfiguredMemberUniverse) {
    SeedChild(11, "child-x", /*score=*/true, /*ctx=*/1);  // no hype block anywhere
    SeedChild(12, "child-y", false, 1);
    auto n = SynthesizeEnrichAuditRows(db_, {"enrich", "contextual"}, 500);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 1);  // only child-y owes enrich; hype not in the universe
    auto rows = ListEnrichStateForDoc(db_, "doc-a", kEnrichStatusPendingRetry);
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(rows.value()[0].failed_members, "enrich");
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
