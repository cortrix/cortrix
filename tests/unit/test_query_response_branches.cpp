// QueryResponse::ToJson — branch coverage for the per-route hit_routes bitmask,
// the per-route raw-score emission, the sql_result has_result / error branches,
// and the L3 has_error branch. All paths are exercised by constructing the value
// struct directly (no I/O, no model) and asserting the serialized JSON shape.
#include <gtest/gtest.h>

#include "cortrix/query/query_response.h"
#include "cortrix/query/scored_block.h"

namespace cortrix {
namespace {

// --- hit_routes bitmask + per-route raw score emission --------------------

TEST(QueryResponseToJsonTest, VectorOnlyRouteEmitsVectorScoreOnly) {
    QueryResponse resp;
    ResultItem item;
    item.block_id = 1;
    item.hit_routes = kRouteVector;     // only the vector bit set
    item.vector_score = 0.42f;
    item.bm25_score = 0.99f;            // present but must NOT be serialized
    resp.results.push_back(item);

    json j = resp.ToJson();
    ASSERT_EQ(j["results"].size(), 1u);
    const json& r = j["results"][0];
    ASSERT_EQ(r["hit_routes"].size(), 1u);
    EXPECT_EQ(r["hit_routes"][0], "vector");
    EXPECT_TRUE(r.contains("vector_score"));
    EXPECT_FALSE(r.contains("bm25_score"));  // bm25 bit not set → key absent
}

TEST(QueryResponseToJsonTest, Bm25OnlyRouteEmitsBm25ScoreOnly) {
    QueryResponse resp;
    ResultItem item;
    item.hit_routes = kRouteBM25;
    item.bm25_score = 0.31f;
    resp.results.push_back(item);

    json j = resp.ToJson();
    const json& r = j["results"][0];
    ASSERT_EQ(r["hit_routes"].size(), 1u);
    EXPECT_EQ(r["hit_routes"][0], "bm25");
    EXPECT_FALSE(r.contains("vector_score"));
    EXPECT_TRUE(r.contains("bm25_score"));
}

TEST(QueryResponseToJsonTest, AllThreeRoutesEmitAllHitsAndBothScores) {
    QueryResponse resp;
    ResultItem item;
    item.hit_routes = kRouteVector | kRouteBM25 | kRouteSQL;
    item.vector_score = 0.5f;
    item.bm25_score = 0.6f;
    resp.results.push_back(item);

    json j = resp.ToJson();
    const json& r = j["results"][0];
    ASSERT_EQ(r["hit_routes"].size(), 3u);
    EXPECT_EQ(r["hit_routes"][0], "vector");
    EXPECT_EQ(r["hit_routes"][1], "bm25");
    EXPECT_EQ(r["hit_routes"][2], "sql");
    EXPECT_TRUE(r.contains("vector_score"));
    EXPECT_TRUE(r.contains("bm25_score"));
}

TEST(QueryResponseToJsonTest, NoRouteHitsEmptyHitArrayNoRawScores) {
    QueryResponse resp;
    ResultItem item;
    item.hit_routes = 0;  // no bits → all per-route branches false
    resp.results.push_back(item);

    json j = resp.ToJson();
    const json& r = j["results"][0];
    EXPECT_TRUE(r["hit_routes"].empty());
    EXPECT_FALSE(r.contains("vector_score"));
    EXPECT_FALSE(r.contains("bm25_score"));
}

// --- sql_result: null vs populated, error-empty vs error-present -----------

TEST(QueryResponseToJsonTest, SqlResultNullWhenNoResult) {
    QueryResponse resp;  // sql_result.has_result defaults false
    json j = resp.ToJson();
    EXPECT_TRUE(j["sql_result"].is_null());
}

TEST(QueryResponseToJsonTest, SqlResultPopulatedWithoutError) {
    QueryResponse resp;
    resp.sql_result.has_result = true;
    resp.sql_result.query = "SELECT 1";
    resp.sql_result.columns = {"n"};
    resp.sql_result.rows = {{json(1)}};
    resp.sql_result.row_count = 1;
    resp.sql_result.truncated = false;
    resp.sql_result.error = "";  // empty → no "error" key

    json j = resp.ToJson();
    ASSERT_FALSE(j["sql_result"].is_null());
    EXPECT_EQ(j["sql_result"]["query"], "SELECT 1");
    EXPECT_EQ(j["sql_result"]["row_count"], 1);
    EXPECT_FALSE(j["sql_result"].contains("error"));  // empty error omitted
}

TEST(QueryResponseToJsonTest, SqlResultPopulatedWithError) {
    QueryResponse resp;
    resp.sql_result.has_result = true;
    resp.sql_result.error = "syntax error near FROM";

    json j = resp.ToJson();
    ASSERT_FALSE(j["sql_result"].is_null());
    EXPECT_EQ(j["sql_result"]["error"], "syntax error near FROM");
}

// --- L3 error branch -------------------------------------------------------

TEST(QueryResponseToJsonTest, ErrorKeyPresentOnlyWhenHasError) {
    QueryResponse ok;
    EXPECT_FALSE(ok.ToJson().contains("error"));

    QueryResponse err;
    err.has_error = true;
    err.error_message = "all routes failed";
    json j = err.ToJson();
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"], "all routes failed");
}

}  // namespace
}  // namespace cortrix
