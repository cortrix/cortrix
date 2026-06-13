#include "cortrix/spc/contextual_store.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/f35_schema_provider.h"

// I2 — WriteContextualized persists F35's contextualized_* columns onto the blocks
// row F35SchemaProvider created. Tests the write, the BLOB round-trip, and the
// no-op-when-no-F35-output path.
namespace cortrix::spc {
namespace {

class ContextualStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        // Minimal blocks table; the F35 provider ALTERs in the contextualized_* cols.
        ASSERT_EQ(sqlite3_exec(db_,
            "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, doc_id TEXT, "
            "content_text TEXT)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        F35SchemaProvider p;
        ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
        ASSERT_EQ(sqlite3_exec(db_,
            "INSERT INTO blocks(block_id, doc_id, content_text) VALUES(7,'d1','txt')",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }
    void TearDown() override { sqlite3_close(db_); }

    int StatusCol(uint64_t id) {
        return ScalarInt("SELECT contextualized_status FROM blocks WHERE block_id=?1", id);
    }
    std::string TextCol(uint64_t id) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT contextualized_text FROM blocks WHERE block_id=?1", -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(id));
        std::string out;
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(s, 0);
            if (t) out = reinterpret_cast<const char*>(t);
        }
        sqlite3_finalize(s);
        return out;
    }
    std::vector<float> EmbCol(uint64_t id) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT contextualized_embedding FROM blocks WHERE block_id=?1", -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(id));
        std::vector<float> out;
        if (sqlite3_step(s) == SQLITE_ROW) {
            const void* blob = sqlite3_column_blob(s, 0);
            int n = sqlite3_column_bytes(s, 0);
            if (blob && n > 0) {
                out.resize(n / sizeof(float));
                std::memcpy(out.data(), blob, out.size() * sizeof(float));
            }
        }
        sqlite3_finalize(s);
        return out;
    }
    int ScalarInt(const char* sql, uint64_t id) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(id));
        int v = -999;
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        return v;
    }

    sqlite3* db_ = nullptr;
};

TEST_F(ContextualStoreTest, WritesTextEmbeddingAndStatus) {
    EnrichResult r;
    r.contextualized_text = "context: txt";
    r.contextualized_embedding = std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f};
    r.contextualized_status = 1;  // generated
    ASSERT_TRUE(WriteContextualized(db_, 7, r).ok());

    EXPECT_EQ(StatusCol(7), 1);
    EXPECT_EQ(TextCol(7), "context: txt");
    auto emb = EmbCol(7);
    ASSERT_EQ(emb.size(), 4u);
    EXPECT_FLOAT_EQ(emb[0], 1.0f);
    EXPECT_FLOAT_EQ(emb[3], 4.0f);
}

TEST_F(ContextualStoreTest, NoOpWhenNoF35Output) {
    EnrichResult r;  // status 0, no contextualized fields → F35 never ran
    ASSERT_TRUE(WriteContextualized(db_, 7, r).ok());
    EXPECT_EQ(StatusCol(7), 0);          // DEFAULT untouched
    EXPECT_EQ(TextCol(7), "");           // NULL
    EXPECT_TRUE(EmbCol(7).empty());
}

TEST_F(ContextualStoreTest, WritesStatusOnFailedDegrade) {
    EnrichResult r;
    r.contextualized_status = 2;  // failed (LLM degrade) — no text/embedding
    ASSERT_TRUE(WriteContextualized(db_, 7, r).ok());
    EXPECT_EQ(StatusCol(7), 2);
    EXPECT_EQ(TextCol(7), "");
    EXPECT_TRUE(EmbCol(7).empty());
}

}  // namespace
}  // namespace cortrix::spc
