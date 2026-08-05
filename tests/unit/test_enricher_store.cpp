#include "cortrix/spc_enricher/enricher_store.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_schema_provider.h"

namespace cortrix::spc {
namespace {

// Build a per-Unit DB with a blocks table + the enricher schema applied, and insert
// one block row to attach enrichment to.
class EnricherStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db_,
            "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "doc_id TEXT, chunk_index INTEGER, content_text TEXT)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        EnricherSchemaProvider p;
        ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
        ASSERT_EQ(sqlite3_exec(db_,
            "INSERT INTO blocks(block_id, doc_id, chunk_index, content_text) "
            "VALUES(1,'d1',0,'hello Cortrix')",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }
    void TearDown() override { sqlite3_close(db_); }

    std::string BlockMeta(uint64_t block_id) {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(
            db_, "SELECT enricher_metadata FROM blocks WHERE block_id=?1", -1, &stmt, nullptr),
            SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, block_id);
        std::string out;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(stmt, 0);
            if (t) out = reinterpret_cast<const char*>(t);
        }
        sqlite3_finalize(stmt);
        return out;
    }
    double BlockScore(uint64_t block_id) {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(
            db_, "SELECT enriched_score FROM blocks WHERE block_id=?1", -1, &stmt, nullptr),
            SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, block_id);
        double v = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_double(stmt, 0);
        sqlite3_finalize(stmt);
        return v;
    }
    int FtsCount(const std::string& query) {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(
            db_, "SELECT COUNT(*) FROM entities_fts WHERE entities_fts MATCH ?1", -1, &stmt, nullptr),
            SQLITE_OK);
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        int n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    }

    sqlite3* db_ = nullptr;
};

EnrichResult MakeResult() {
    EnrichResult r;
    r.status = 0;
    r.enriched_score = 0.85f;
    r.enriched_at = 1700000000000;
    r.summary = "doc about Cortrix";
    r.model_used = "gpt-4o-mini";
    r.prompt_version = "default-zh";
    r.enricher_name = "LlmEnricher";
    r.token_count = 42;
    r.duration_ms = 120;
    r.entities = {
        {"Cortrix", "PRODUCT", 6, 13},
        {"Acme Corp", "ORG", 0, 9},
    };
    return r;
}

TEST_F(EnricherStoreTest, WritesBlockColumnsAndMetadata) {
    ASSERT_TRUE(WriteBlockEnrichment(db_, 1, MakeResult()).ok());
    // enriched_score is float (0.85f) widened to a SQLite REAL/double on read —
    // compare with a float-precision tolerance, not bit-exact double equality.
    EXPECT_NEAR(BlockScore(1), 0.85, 1e-6);

    auto meta = nlohmann::json::parse(BlockMeta(1));
    EXPECT_EQ(meta["model_used"], "gpt-4o-mini");
    EXPECT_EQ(meta["prompt_version"], "default-zh");
    EXPECT_EQ(meta["enricher_name"], "LlmEnricher");
    EXPECT_EQ(meta["token_count"], 42);
}

TEST_F(EnricherStoreTest, FailedResultWritesNoEnrichment) {
    EnrichResult bad;
    bad.status = 2;  // CX_ERR_ENRICHER_LLM_API
    ASSERT_TRUE(WriteBlockEnrichment(db_, 1, bad).ok());
    EXPECT_DOUBLE_EQ(BlockScore(1), 0.0);  // column stays NULL → read as 0.0
    EXPECT_TRUE(BlockMeta(1).empty());
}

TEST_F(EnricherStoreTest, WritesEntitiesAndReadsBack) {
    ASSERT_TRUE(WriteEntities(db_, 1, MakeResult().entities).ok());
    auto got = ReadEntities(db_, 1);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].text, "Cortrix");
    EXPECT_EQ(got[0].type, "PRODUCT");
    EXPECT_EQ(got[0].start_offset, 6);
    EXPECT_EQ(got[1].text, "Acme Corp");
    EXPECT_EQ(got[1].type, "ORG");
}

TEST_F(EnricherStoreTest, EntitiesFtsSearchable) {
    ASSERT_TRUE(WriteEntities(db_, 1, MakeResult().entities).ok());
    // FTS5 over (text, type) — entity text + type are searchable.
    EXPECT_EQ(FtsCount("Cortrix"), 1);
    EXPECT_EQ(FtsCount("PRODUCT"), 1);
    EXPECT_EQ(FtsCount("ORG"), 1);
    EXPECT_EQ(FtsCount("Nonexistent"), 0);
}

TEST_F(EnricherStoreTest, WriteEnrichmentTransactionalCombined) {
    ASSERT_TRUE(WriteEnrichment(db_, 1, MakeResult()).ok());
    EXPECT_NEAR(BlockScore(1), 0.85, 1e-6);
    EXPECT_EQ(ReadEntities(db_, 1).size(), 2u);
    EXPECT_EQ(FtsCount("Cortrix"), 1);
}

TEST_F(EnricherStoreTest, EmptyEntitiesNoOp) {
    EXPECT_TRUE(WriteEntities(db_, 1, {}).ok());
    EXPECT_TRUE(ReadEntities(db_, 1).empty());
}

// ============================================================
// Branch coverage: null-db guards / NullEnricher empty-success / SQL prepare
// failures / WriteEnrichment rollback.
// ============================================================

// Every public entry rejects a null db with InvalidArgument (the !db guards).
TEST(EnricherStoreNullDbTest, NullDbGuards) {
    EnrichResult r;
    r.status = 0;
    r.enricher_name = "LlmEnricher";
    EXPECT_FALSE(WriteBlockEnrichment(nullptr, 1, r).ok());
    EXPECT_FALSE(WriteEntities(nullptr, 1, {{"X", "ORG", 0, 1}}).ok());
    EXPECT_FALSE(WriteEnrichment(nullptr, 1, r).ok());
    // ReadEntities returns an empty vector (not a Status) on null db.
    EXPECT_TRUE(ReadEntities(nullptr, 1).empty());
}

// A status==0 result with an EMPTY enricher_name (the NullEnricher empty-success
// shape) writes nothing — the `|| result.enricher_name.empty()` arm of the early
// return, distinct from the failed-result arm covered above.
TEST_F(EnricherStoreTest, NullEnricherEmptyNameWritesNothing) {
    EnrichResult empty_ok;
    empty_ok.status = 0;             // success
    empty_ok.enricher_name = "";     // but no enricher actually ran
    empty_ok.enriched_score = 0.0f;
    ASSERT_TRUE(WriteBlockEnrichment(db_, 1, empty_ok).ok());
    EXPECT_DOUBLE_EQ(BlockScore(1), 0.0);  // column stays NULL
    EXPECT_TRUE(BlockMeta(1).empty());
}

// WriteEntities against a db WITHOUT the entities table → the first prepare fails
// → SqlErr (the prepare-insert-entities !=SQLITE_OK branch).
TEST(EnricherStoreNoSchemaTest, WriteEntitiesPrepareFails) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);  // no enricher schema applied
    Status s = WriteEntities(db, 1, {{"X", "ORG", 0, 1}});
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_ENRICHER_STORE"), std::string::npos);
    sqlite3_close(db);
}

// ReadEntities against a db without the entities table → prepare fails → empty
// vector (the prepare !=SQLITE_OK early-return branch).
TEST(EnricherStoreNoSchemaTest, ReadEntitiesPrepareFailsReturnsEmpty) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_TRUE(ReadEntities(db, 1).empty());
    sqlite3_close(db);
}

// WriteEnrichment wraps both writes in a txn; when WriteEntities fails (no
// entities table) the combined call rolls back and the block columns are NOT
// committed (the s.ok() false branch → ROLLBACK).
TEST(EnricherStoreNoSchemaTest, WriteEnrichmentRollsBackOnEntityFailure) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    // A blocks table with the enricher columns but NO entities table → block write
    // succeeds, entity write fails → whole txn rolls back.
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, enriched_score REAL, "
        "enriched_at INTEGER, enricher_metadata TEXT)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "INSERT INTO blocks(block_id) VALUES(1)",
        nullptr, nullptr, nullptr), SQLITE_OK);

    EnrichResult r = MakeResult();  // has entities → WriteEntities will be attempted
    Status s = WriteEnrichment(db, 1, r);
    EXPECT_FALSE(s.ok());

    // Rolled back → enriched_score stays NULL.
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        db, "SELECT enriched_score FROM blocks WHERE block_id=1", -1, &st, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_type(st, 0), SQLITE_NULL);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// WriteEnrichment with a result that has NO entities → WriteBlockEnrichment runs,
// WriteEntities is a no-op (empty vector), txn commits (the entities-empty +
// commit branch).
TEST_F(EnricherStoreTest, WriteEnrichmentNoEntitiesCommits) {
    EnrichResult r = MakeResult();
    r.entities.clear();
    ASSERT_TRUE(WriteEnrichment(db_, 1, r).ok());
    EXPECT_NEAR(BlockScore(1), 0.85, 1e-6);
    EXPECT_TRUE(ReadEntities(db_, 1).empty());
}

}  // namespace
}  // namespace cortrix::spc
