#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/llm/i_llm_client.h"
#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_config_resolver.h"
#include "cortrix/spc_enricher/enricher_store.h"
#include "cortrix/spc_enricher/enricher_schema_provider.h"
#include "cortrix/spc/parser.h"
#include "mock_llm_client.h"
#include "mock_response_builder.h"

// Enricher standalone integration (Issue 6.5 E2E within-feature): resolve NS config →
// enrich a batch through the (mocked) LLM → persist to the per-Unit store →
// read back + FTS5 search. The CROSS-feature E2E (real SPC pipeline Parse→Chunk→
// Enricher→Embed→Store, real LLM) is integration deferred wiring.
namespace cortrix::spc {
namespace {

using ::testing::_;
using ::testing::Return;

class EnricherIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db_,
            "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "doc_id TEXT, chunk_index INTEGER, content_text TEXT)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        EnricherSchemaProvider p;
        ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
    }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
};

TEST_F(EnricherIntegrationTest, ResolveEnrichPersistSearchRoundTrip) {
    // 1) Resolve effective config: global + NS override (model + template).
    EnricherConfig global;
    global.type = EnricherType::kLlm;
    global.enabled = true;
    global.model = "gpt-4o-mini";
    global.batch_size = 8;
    global.circuit_breaker_enabled = false;
    EnricherConfigResolver resolver(global);
    auto eff = resolver.Resolve(std::string(R"({"model":"gpt-4o","prompt_template_id":"default-en"})"));
    ASSERT_TRUE(eff.ok());
    EXPECT_EQ(eff.value().model, "gpt-4o");

    // 2) Enrich a 2-chunk batch through the mocked LLM (Normal builder).
    auto mock = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(llm::MockResponseBuilder::Normal(2, "gpt-4o")));
    LlmEnricher enr(eff.value(), mock);

    DocumentMetadata meta;
    meta.filename = "doc.pdf";
    std::vector<ChunkContext> batch;
    for (int i = 0; i < 2; ++i) {
        // Insert a backing block row per chunk so the store has a target.
        std::string sql = "INSERT INTO blocks(doc_id, chunk_index, content_text) "
                          "VALUES('d1'," + std::to_string(i) + ",'text " + std::to_string(i) + "')";
        ASSERT_EQ(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        ChunkContext c;
        c.chunk_text = "text " + std::to_string(i);
        c.chunk_index = i;
        c.doc_metadata = &meta;
        batch.push_back(c);
    }
    auto results = enr.EnrichBatch(batch);
    ASSERT_EQ(results.size(), 2u);
    ASSERT_TRUE(results[0].ok());
    ASSERT_TRUE(results[1].ok());

    // 3) Persist each result to its block (block_id 1-based matches insert order).
    for (size_t i = 0; i < results.size(); ++i) {
        ASSERT_TRUE(WriteEnrichment(db_, static_cast<int64_t>(i + 1), results[i]).ok());
    }

    // 4) Read back entities + FTS5 search across the unit.
    auto e0 = ReadEntities(db_, 1);
    ASSERT_EQ(e0.size(), 1u);
    EXPECT_EQ(e0[0].type, "ORG");

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        db_, "SELECT COUNT(*) FROM entities_fts WHERE entities_fts MATCH 'ORG'", -1, &stmt, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);  // both chunks' entities indexed
    sqlite3_finalize(stmt);

    // 5) blocks.enriched_score populated for both blocks.
    sqlite3_stmt* s2 = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        db_, "SELECT COUNT(*) FROM blocks WHERE enriched_score IS NOT NULL", -1, &s2, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(s2), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(s2, 0), 2);
    sqlite3_finalize(s2);
}

TEST_F(EnricherIntegrationTest, NullEnricherPathPersistsNothing) {
    // type=null → NullEnricher; results are empty-success → store writes no
    // enrichment (columns stay NULL), matching the scenario-1 normal path.
    EnricherConfig cfg;  // kNull
    auto enr = CreateEnricher(cfg);  // single-arg (no probe needed for null)
    EXPECT_EQ(enr->Name(), "NullEnricher");

    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO blocks(block_id, doc_id, chunk_index, content_text) "
        "VALUES(1,'d1',0,'x')", nullptr, nullptr, nullptr), SQLITE_OK);
    DocumentMetadata meta;
    ChunkContext c;
    c.chunk_text = "x";
    c.doc_metadata = &meta;
    auto results = enr->EnrichBatch({c});
    ASSERT_EQ(results.size(), 1u);
    ASSERT_TRUE(WriteEnrichment(db_, 1, results[0]).ok());  // empty result → no-op write

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        db_, "SELECT enriched_score FROM blocks WHERE block_id=1", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_type(stmt, 0), SQLITE_NULL);  // stayed NULL
    sqlite3_finalize(stmt);
}

}  // namespace
}  // namespace cortrix::spc
