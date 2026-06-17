// R8 branch-coverage supplement for src/query/query_request.cpp (27 missed @ 73.5%).
//
// The existing test_query_request.cpp covers FromJson complete/minimal/not-object,
// the full Validate rejection set, all Normalize cases, and the doc_id filter. The
// residual uncovered branches are the FromJson per-field guards that the existing
// tests never toggle individually:
//   - the optional fields the "Complete" test omits entirely: exclude_types,
//     explain, route, granularity (their `j.contains && is_X` TRUE arms);
//   - the WRONG-TYPE false arms of the `&& j[field].is_X()` guards (field present
//     but the wrong JSON type → the field is skipped, default kept);
//   - the per-element `is_string()` skip inside the block_type / exclude_types
//     array loops (a non-string element is dropped);
//   - the filters / search_config "present but not an object" false arms.
// All deterministic, pure JSON in → struct out. No fixture needed.
//
// Standalone NEW file; touches no existing test.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cortrix/query/query_request.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// ---- Optional fields the "Complete" test omits: TRUE arms ----

// exclude_types array + explain + route + granularity all present & correct → each
// field's `contains && is_X` TRUE arm fires and the value is read.
TEST(QueryRequestR8, FromJson_AllOptionalFieldsParsed) {
    json j = {
        {"query", "q"}, {"namespace", "ns"},
        {"filters", {
            {"exclude_types", {"FILE", "MEMORY"}},
            {"doc_id", "01JABC"},
        }},
        {"explain", true},
        {"route", "force_complex"},
        {"granularity", "doc"},
    };
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    ASSERT_EQ(req.filters.exclude_types.size(), 2u);
    EXPECT_EQ(req.filters.exclude_types[0], "FILE");
    EXPECT_EQ(req.filters.exclude_types[1], "MEMORY");
    EXPECT_TRUE(req.explain);
    EXPECT_EQ(req.route, "force_complex");
    EXPECT_EQ(req.granularity, "doc");
}

// ---- WRONG-TYPE false arms: each `&& is_X()` guard's false side ----

// top_k present but a STRING (not integer) → the is_number_integer() false arm →
// top_k keeps its default (10). Same for timeout_ms (string → default 5000).
TEST(QueryRequestR8, FromJson_NumericFieldsWrongTypeIgnored) {
    json j = {
        {"query", "q"}, {"namespace", "ns"},
        {"top_k", "five"},        // wrong type → ignored
        {"timeout_ms", "later"},  // wrong type → ignored
    };
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    EXPECT_EQ(req.top_k, 10);       // default kept
    EXPECT_EQ(req.timeout_ms, 5000);
}

// query / namespace present but NOT strings → the is_string() false arms → left
// empty (Validate would later reject, but FromJson itself just skips).
TEST(QueryRequestR8, FromJson_QueryNamespaceWrongTypeIgnored) {
    json j = {{"query", 123}, {"namespace", false}};
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());  // FromJson is lenient
    EXPECT_TRUE(req.query.empty());
    EXPECT_TRUE(req.namespace_name.empty());
}

// filters present but NOT an object (an array) → the `filters.is_object()` false
// arm → the whole filters block is skipped.
TEST(QueryRequestR8, FromJson_FiltersNotObjectIgnored) {
    json j = {{"query", "q"}, {"namespace", "ns"}, {"filters", json::array({1, 2})}};
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    EXPECT_TRUE(req.filters.block_types.empty());
    EXPECT_TRUE(req.filters.exclude_types.empty());
}

// block_type present but NOT an array (a string) → the is_array() false arm; and
// inside a valid array, a NON-STRING element → the per-element is_string() false
// arm (the element is dropped, the string ones kept).
TEST(QueryRequestR8, FromJson_BlockTypeArrayElementTypeArms) {
    // block_type a bare string → is_array() false → no block_types parsed.
    json j1 = {{"query", "q"}, {"namespace", "ns"},
               {"filters", {{"block_type", "FILE"}}}};
    QueryRequest r1;
    ASSERT_TRUE(QueryRequest::FromJson(j1, &r1).ok());
    EXPECT_TRUE(r1.filters.block_types.empty());

    // block_type array with a mix of string + non-string → only strings survive.
    json j2 = {{"query", "q"}, {"namespace", "ns"},
               {"filters", {{"block_type", json::array({"FILE", 42, "DATABASE"})}}}};
    QueryRequest r2;
    ASSERT_TRUE(QueryRequest::FromJson(j2, &r2).ok());
    ASSERT_EQ(r2.filters.block_types.size(), 2u);  // 42 dropped
    EXPECT_EQ(r2.filters.block_types[0], "FILE");
    EXPECT_EQ(r2.filters.block_types[1], "DATABASE");
}

// exclude_types: same is_array() false arm + per-element is_string() skip.
TEST(QueryRequestR8, FromJson_ExcludeTypesArrayElementArms) {
    json j = {{"query", "q"}, {"namespace", "ns"},
              {"filters", {{"exclude_types", json::array({"FILE", true, "MEMORY"})}}}};
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    ASSERT_EQ(req.filters.exclude_types.size(), 2u);  // `true` dropped
    EXPECT_EQ(req.filters.exclude_types[0], "FILE");
    EXPECT_EQ(req.filters.exclude_types[1], "MEMORY");
}

// filters string sub-fields present but wrong type → is_string() false arms
// (source_path / created_after / doc_id left at defaults).
TEST(QueryRequestR8, FromJson_FilterStringFieldsWrongTypeIgnored) {
    json j = {{"query", "q"}, {"namespace", "ns"},
              {"filters", {
                  {"source_path", 123},       // wrong type
                  {"created_after", false},   // wrong type
                  {"doc_id", json::array()},  // wrong type
              }}};
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    EXPECT_TRUE(req.filters.source_path.empty());
    EXPECT_TRUE(req.filters.created_after.empty());
}

// explain / route / granularity present but wrong type → their is_boolean() /
// is_string() false arms (defaults kept: explain=false, route/granularity empty).
TEST(QueryRequestR8, FromJson_ExplainRouteGranularityWrongTypeIgnored) {
    json j = {{"query", "q"}, {"namespace", "ns"},
              {"explain", "yes"},        // not a boolean → ignored
              {"route", 7},              // not a string → ignored
              {"granularity", false}};   // not a string → ignored
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    EXPECT_FALSE(req.explain);
    EXPECT_EQ(req.route, "auto");         // wrong-type ignored → keeps the "auto" default
    EXPECT_EQ(req.granularity, "auto");   // wrong-type ignored → keeps the "auto" default
}

// search_config present but NOT an object → the is_object() false arm (the whole
// block skipped → the three enable_* keep their true defaults).
TEST(QueryRequestR8, FromJson_SearchConfigNotObjectIgnored) {
    json j = {{"query", "q"}, {"namespace", "ns"},
              {"search_config", "all"}};  // not an object
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    EXPECT_TRUE(req.search_config.enable_vector);
    EXPECT_TRUE(req.search_config.enable_bm25);
    EXPECT_TRUE(req.search_config.enable_sql);
}

// search_config sub-fields present but wrong type → each is_boolean() false arm
// (defaults kept). Distinct from the not-object case above.
TEST(QueryRequestR8, FromJson_SearchConfigFieldsWrongTypeIgnored) {
    json j = {{"query", "q"}, {"namespace", "ns"},
              {"search_config", {
                  {"enable_vector", "on"},   // wrong type
                  {"enable_bm25", 1},        // wrong type (int, not bool)
                  {"enable_sql", json::array()},
              }}};
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    EXPECT_TRUE(req.search_config.enable_vector);  // all defaults kept
    EXPECT_TRUE(req.search_config.enable_bm25);
    EXPECT_TRUE(req.search_config.enable_sql);
}

// search_config with CORRECT-type values that flip the defaults (the is_boolean()
// TRUE arms for all three — the "Complete" test sets vector=true/bm25,sql=false;
// this flips vector=false to cover that side too).
TEST(QueryRequestR8, FromJson_SearchConfigAllFlipped) {
    json j = {{"query", "q"}, {"namespace", "ns"},
              {"search_config", {
                  {"enable_vector", false},
                  {"enable_bm25", true},
                  {"enable_sql", true},
              }}};
    QueryRequest req;
    ASSERT_TRUE(QueryRequest::FromJson(j, &req).ok());
    EXPECT_FALSE(req.search_config.enable_vector);
    EXPECT_TRUE(req.search_config.enable_bm25);
    EXPECT_TRUE(req.search_config.enable_sql);
}

// Validate: the exclude_types invalid-value rejection arm (the existing test covers
// invalid block_type but NOT invalid exclude_type).
TEST(QueryRequestR8, Validate_InvalidExcludeType) {
    QueryRequest req;
    req.query = "q";
    req.namespace_name = "ns";
    req.filters.exclude_types.push_back("NOT_A_REAL_TYPE");
    Status st = req.Validate();
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("invalid exclude_type"), std::string::npos);
}

// Validate: a valid exclude_type passes the loop (the BlockTypeFromString >= 0 arm
// for exclude_types specifically).
TEST(QueryRequestR8, Validate_ValidExcludeTypePasses) {
    QueryRequest req;
    req.query = "q";
    req.namespace_name = "ns";
    req.filters.exclude_types.push_back("FILE");
    EXPECT_TRUE(req.Validate().ok());
}

}  // namespace
}  // namespace cortrix
