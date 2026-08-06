#include "cortrix/retrieval/sparse_vec_store.h"

#include <gtest/gtest.h>

#include <vector>

#include <sqlite3.h>

#include "cortrix/retrieval/sparse_schema_provider.h"
#include "cortrix/retrieval/sparse_codec.h"

// Q4 — WriteSparseVec persists a child's SPLADE sparse vector into the
// blocks.sparse_vec BLOB column SparseSchemaProvider adds. Tests the write, the BLOB
// round-trip through the codec, and the empty-vector → SQL NULL (dead chunk,
//) path.
namespace cortrix::retrieval {
namespace {

class SparseVecStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db_,
            "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, child_id TEXT)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        SparseSchemaProvider p;
        ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());  // adds blocks.sparse_vec + inverted index
        ASSERT_EQ(sqlite3_exec(db_,
            "INSERT INTO blocks(block_id, child_id) VALUES(7,'c1')",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }
    void TearDown() override { sqlite3_close(db_); }

    // Read sparse_vec for a block; returns {is_null, decoded_vector}.
    std::pair<bool, SparseVector> ReadSparse(uint64_t id) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_, "SELECT sparse_vec FROM blocks WHERE block_id=?1", -1,
                           &s, nullptr);
        sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(id));
        std::pair<bool, SparseVector> out{true, {}};
        if (sqlite3_step(s) == SQLITE_ROW) {
            if (sqlite3_column_type(s, 0) == SQLITE_NULL) {
                out.first = true;  // NULL
            } else {
                out.first = false;
                const auto* data =
                    static_cast<const uint8_t*>(sqlite3_column_blob(s, 0));
                const int len = sqlite3_column_bytes(s, 0);
                auto r = DeserializeSparseVec(data, static_cast<size_t>(len));
                if (r.ok()) out.second = r.value();
            }
        }
        sqlite3_finalize(s);
        return out;
    }

    sqlite3* db_ = nullptr;
};

TEST_F(SparseVecStoreTest, WritesAndRoundTripsSparseVector) {
    SparseVector v;
    v.terms = {{10u, 0.5f}, {200u, 0.25f}, {3000u, 0.125f}};
    ASSERT_TRUE(WriteSparseVec(db_, 7, v).ok());

    auto [is_null, got] = ReadSparse(7);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(got.terms, v.terms);  // exact round-trip (term_id + weight)
}

TEST_F(SparseVecStoreTest, EmptyVectorWritesSqlNull) {
    SparseVector empty;
    ASSERT_TRUE(WriteSparseVec(db_, 7, empty).ok());

    auto [is_null, got] = ReadSparse(7);
    EXPECT_TRUE(is_null);          // dead chunk → NULL, not an all-zero BLOB
    EXPECT_TRUE(got.terms.empty());
}

TEST_F(SparseVecStoreTest, NullHandleIsAnError) {
    SparseVector v;
    v.terms = {{1u, 1.0f}};
    EXPECT_FALSE(WriteSparseVec(nullptr, 7, v).ok());
}

}  // namespace
}  // namespace cortrix::retrieval
