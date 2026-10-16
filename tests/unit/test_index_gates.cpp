#include <gtest/gtest.h>
#include <sqlite3.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/spc/data_cleaner.h"  // ShouldSkipIndex, spc::Block, kBlockFlagExtAnomalous
#include "cortrix/spc/index_gates.h"

namespace cortrix::spc {
namespace {

// The SQL form of the mark over one stored metadata_json value (SQL NULL when
// `text` is nullptr), read the way the audit reads it: sqlite3_column_int, which
// turns a NULL result into 0.
int SqlSkipIndex(sqlite3* db, const char* text) {
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string("SELECT ") + kBlockSkipsIndexSql +
                            " FROM (SELECT ?1 AS metadata_json)";
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    if (text) sqlite3_bind_text(st, 1, text, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 1);
    const int rc = sqlite3_step(st);
    EXPECT_EQ(rc, SQLITE_ROW) << "the predicate must never raise: " << sqlite3_errmsg(db);
    const int v = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

// The two predicates are the ingest's gates verbatim: points (primary and
// contextual, with the label) only at the full level and never for a skip-index
// chunk; hype blocks only at the full level, whatever the cleaner said.
TEST(IndexGatesTest, PredicatesFollowTheIngestRule) {
    for (int level = 0; level <= 3; ++level) {
        for (bool skip : {false, true}) {
            EXPECT_EQ(IndexPointsExpected(level, skip), level >= 3 && !skip)
                << "level " << level << " skip " << skip;
        }
        EXPECT_EQ(HypeBlocksExpected(level), level >= 3) << "level " << level;
    }
}

// The three readings of the cleaner's mark — the in-memory Block, the stored text,
// and the SQL column — agree on every shape a stored metadata_json can take. Only
// the JSON boolean true is the mark; malformed text and NULL read as false and
// never make the SQL raise.
TEST(IndexGatesTest, MetadataHelperAgreesWithSqlPredicateAndCleaner) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    struct Sample {
        const char* text;
        bool skip;
    };
    const std::vector<Sample> samples = {
        {R"({"cleaning.skip_index":true})", true},
        {R"({"cleaning.anomaly_reason":"oversized","cleaning.skip_index":true})", true},
        {R"({"cleaning.skip_index":true,"summary":"x"})", true},  // after a summary backfill
        {R"( {"cleaning.skip_index":true} )", true},               // surrounding blanks
        {R"({"cleaning.skip_index":false})", false},
        {R"({"a":1})", false},                                     // key absent
        {R"({"cleaning.skip_index":"true"})", false},              // a string is not the mark
        {R"({"cleaning.skip_index":1})", false},                   // a number is not the mark
        {R"({"cleaning.skip_index":null})", false},
        {R"({})", false},
        {R"([])", false},
        {"true", false},                                           // top-level scalars
        {"1", false},
        {R"("x")", false},
        {"{", false},                                              // malformed
        {"", false},
        {nullptr, false},                                          // SQL NULL
    };
    for (const auto& s : samples) {
        const std::string shown = s.text ? s.text : "<NULL>";
        EXPECT_EQ(MetadataSkipsIndex(s.text ? s.text : ""), s.skip) << shown;
        EXPECT_EQ(SqlSkipIndex(db, s.text), s.skip ? 1 : 0) << shown;
        if (s.text) {
            // The cleaner's in-memory reading with the header bit clear — blocks
            // written by a version that does not persist the bit carry the key
            // alone, so the stored forms cannot rely on the bit.
            Block b;
            b.metadata_json =
                nlohmann::json::parse(s.text, nullptr, /*allow_exceptions=*/false);
            EXPECT_EQ(ShouldSkipIndex(b), s.skip) << shown;
        }
    }
    // The in-memory form alone also honours the header bit (the cleaner sets both).
    Block flagged;
    flagged.flags_ext = kBlockFlagExtAnomalous;
    EXPECT_TRUE(ShouldSkipIndex(flagged));
    EXPECT_FALSE(MetadataSkipsIndex(flagged.metadata_json.dump()));
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::spc
