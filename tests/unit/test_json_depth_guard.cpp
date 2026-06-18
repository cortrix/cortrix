// R9 robustness -- unit tests for the metadata depth guard (json_depth.h) and the
// read-side defense in FlattenMetadataIntoMap.
//
// Background: nlohmann json::dump() (and deep-tree teardown) recurse per nesting
// level with no bound, so deeply-nested user metadata can overflow the stack and
// crash with a SIGSEGV that no try/catch can stop. The fix rejects over-deep metadata
// at every ingest boundary (system-wide "depth <= kMaxMetadataDepth" invariant) and
// adds a defensive check in FlattenMetadataIntoMap for already-stored data.
//
// This file covers the pure helpers + the read-side flatten guard. The HTTP write
// routes (4xx, no crash) are covered in tests/security/test_malformed_input.cpp.

#include <gtest/gtest.h>

#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/json_depth.h"
#include "cortrix/query/live_single_unit_executor.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// Build a JSON string nesting `depth` objects: {"a":{"a":...{"a":1}...}}.
std::string DeepObjectJson(int depth) {
    std::string s;
    s.reserve(static_cast<size_t>(depth) * 6 + 8);
    for (int i = 0; i < depth; ++i) s += "{\"a\":";
    s += "1";
    for (int i = 0; i < depth; ++i) s += "}";
    return s;
}

// --- JsonMaxDepth / JsonExceedsMaxDepth correctness --------------------------------

TEST(JsonDepthGuard, ScalarsAndEmptyContainersAreDepthZero) {
    EXPECT_EQ(JsonMaxDepth(json(42)), 0);
    EXPECT_EQ(JsonMaxDepth(json("str")), 0);
    EXPECT_EQ(JsonMaxDepth(json(nullptr)), 0);
    EXPECT_EQ(JsonMaxDepth(json::object()), 0);
    EXPECT_EQ(JsonMaxDepth(json::array()), 0);
}

TEST(JsonDepthGuard, CountsObjectAndArrayNesting) {
    EXPECT_EQ(JsonMaxDepth(json::parse(R"({"a":1})")), 1);
    EXPECT_EQ(JsonMaxDepth(json::parse(R"({"a":{"b":1}})")), 2);
    EXPECT_EQ(JsonMaxDepth(json::parse(R"([[[]]])")), 2);   // outer/mid/inner-empty
    EXPECT_EQ(JsonMaxDepth(json::parse(R"({"a":[{"b":2}]})")), 3);
}

TEST(JsonDepthGuard, ReportsDeepestBranch) {
    // Mixed-depth object: the deepest path wins.
    auto j = json::parse(R"({"shallow":1,"deep":{"x":{"y":{"z":1}}}})");
    EXPECT_EQ(JsonMaxDepth(j), 4);
}

TEST(JsonDepthGuard, ExceedsBoundaryIsInclusive) {
    // depth == kMaxMetadataDepth is allowed; depth == kMaxMetadataDepth + 1 is not.
    auto at_limit = json::parse(DeepObjectJson(kMaxMetadataDepth));
    auto over_limit = json::parse(DeepObjectJson(kMaxMetadataDepth + 1));
    EXPECT_FALSE(JsonExceedsMaxDepth(at_limit));
    EXPECT_TRUE(JsonExceedsMaxDepth(over_limit));
}

TEST(JsonDepthGuard, EarlyOutDoesNotOverflowOnPathologicalDepth) {
    // The guard itself must survive depth that would overflow dump(). 200k deep.
    auto deep = json::parse(DeepObjectJson(200000));
    EXPECT_TRUE(JsonExceedsMaxDepth(deep));
    // Early-out returns a value strictly greater than the cap, not the true depth.
    EXPECT_GT(JsonMaxDepth(deep, kMaxMetadataDepth), kMaxMetadataDepth);
}

// --- MakeMetadataTooDeepError shape (GEN-Agent machine-readable) -------------------

TEST(JsonDepthGuard, ErrorCarriesCodeCategoryAndStructuredData) {
    auto err = MakeMetadataTooDeepError(/*actual_depth=*/123);
    EXPECT_EQ(err.code, "CX_ERR_METADATA_TOO_DEEP");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, agent_friendly::ErrorCategory::kPermanent);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["max_depth"], kMaxMetadataDepth);
    EXPECT_EQ((*err.structured_data)["actual_depth"], 123);
}

// --- FlattenMetadataIntoMap read-side defense --------------------------------------

TEST(JsonDepthGuard, FlattenValidMetadataStillWorks) {
    std::map<std::string, std::string> out;
    query::FlattenMetadataIntoMap(R"({"beir_corpus_id":"doc-7","score":0.9})", out);
    EXPECT_EQ(out["beir_corpus_id"], "doc-7");  // string verbatim
    EXPECT_EQ(out["score"], "0.9");             // non-string via dump()
}

TEST(JsonDepthGuard, FlattenDeepMetadataDoesNotCrashAndSkips) {
    // The whole point: a deeply-nested stored metadata_json must NOT overflow the
    // stack inside dump(). With the guard, the value is skipped (map left empty)
    // rather than crashing the query worker. 200k deep -> would SIGSEGV without it.
    std::map<std::string, std::string> out;
    query::FlattenMetadataIntoMap(DeepObjectJson(200000), out);
    EXPECT_TRUE(out.empty());  // skipped, no crash
}

TEST(JsonDepthGuard, FlattenAtLimitMetadataIsProcessed) {
    // A value exactly at the limit is still flattened (boundary is inclusive). Wrap
    // the deep chain under a top-level key so the object has a flattenable entry.
    const std::string deep_value = DeepObjectJson(kMaxMetadataDepth - 1);
    std::map<std::string, std::string> out;
    query::FlattenMetadataIntoMap("{\"k\":" + deep_value + "}", out);
    EXPECT_EQ(out.count("k"), 1u);  // depth == kMaxMetadataDepth: allowed, dumped
}

}  // namespace
}  // namespace cortrix
