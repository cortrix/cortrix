// Deterministic tie-break behavior across the two RRF fusers (F36 RagFusion over
// the ChildId keyspace, and the frozen cortrix::RRFFusion over the int64 block_id
// keyspace).
//
// Contract under test (the thing that must NEVER flake): when two results carry an
// EQUAL fused RRF score, the output order must be a stable function of the input,
// not of unordered_map iteration order. RagFusion::FuseResults pins this with an
// explicit `first_seen` (insertion-order) secondary key in its comparator; these
// tests lock that determinism so a refactor that drops the tiebreak is caught.
//
// NOTE (honest scope): the design phrasing "equal score -> child_id ascending" is
// NOT what FuseResults implements -- it breaks ties by first-seen insertion order
// (rag_fusion.cpp comparator). We test the REAL, implemented tiebreak (stable across
// runs), and assert the weaker invariant that frozen cortrix::RRFFusion guarantees.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/query/query_variant_generator.h"
#include "cortrix/query/rag_fusion.h"
#include "cortrix/query/route_result.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/scored_block.h"
#include "cortrix/retrieval/types.h"

namespace cortrix::query {
namespace {

using retrieval::ScoredResult;

std::shared_ptr<RagFusion> MakeFusion() {
    auto gen = std::make_shared<QueryVariantGenerator>(/*llm=*/nullptr);
    auto rrf = std::make_shared<RRFFusion>(60);
    return std::make_shared<RagFusion>(gen, rrf);
}

// --- RagFusion::FuseResults: equal RRF score -> insertion-order tiebreak ------

// Three child_ids each hit at rank-1 in exactly one variant => identical RRF score
// 1/(60+1). The output order is the order in which they were first seen, which is
// the variant/list traversal order. This is deterministic and run-independent.
TEST(QueryTiebreakTest, FuseResultsEqualScoreKeepsInsertionOrder) {
    auto svc = MakeFusion();
    std::vector<std::vector<ScoredResult>> per_variant = {
        {{"zeta", 0.9f}},   // first seen: zeta
        {{"alpha", 0.9f}},  // then alpha
        {{"mike", 0.9f}},   // then mike
    };
    auto out = svc->FuseResults(per_variant, 60);
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value().size(), 3u);
    // All three tie on score; order is first-seen, NOT lexicographic.
    EXPECT_EQ(out.value()[0].child_id, "zeta");
    EXPECT_EQ(out.value()[1].child_id, "alpha");
    EXPECT_EQ(out.value()[2].child_id, "mike");
    const double expected = 1.0 / 61.0;
    for (const auto& r : out.value()) EXPECT_NEAR(r.score, expected, 1e-6);
}

// Determinism across repeated runs: the same input always yields the same order
// (guards against a regression to raw unordered_map iteration order).
TEST(QueryTiebreakTest, FuseResultsTiebreakIsStableAcrossRuns) {
    // Three single-hit variants, all rank-1 => identical RRF score. The tiebreak is
    // first-seen order; the point of this test is run-to-run stability.
    std::vector<std::vector<ScoredResult>> per_variant = {
        {{"c3", 0.9f}},
        {{"c1", 0.9f}},
        {{"c2", 0.9f}},
    };
    auto svc = MakeFusion();
    std::vector<std::string> first;
    // Bind the Result to a named variable: iterating `.value()` of a temporary would
    // dangle (the Result is destroyed before the range-for body runs, pre-C++23).
    auto out1 = svc->FuseResults(per_variant, 60);
    for (const auto& r : out1.value()) {
        first.push_back(r.child_id);
    }
    // Re-run with a fresh service: identical order.
    auto svc2 = MakeFusion();
    std::vector<std::string> second;
    auto out2 = svc2->FuseResults(per_variant, 60);
    for (const auto& r : out2.value()) {
        second.push_back(r.child_id);
    }
    EXPECT_EQ(first, second);
    ASSERT_EQ(first.size(), 3u);
    EXPECT_EQ(first[0], "c3");  // first-seen variant wins the tie
}

// A genuine tie produced by two variants contributing symmetric ranks: child "a"
// (rank1 then rank2) and child "b" (rank2 then rank1) sum to the SAME RRF score.
// The tiebreak resolves to first-seen ("a" appears at list[0] position 0 first).
TEST(QueryTiebreakTest, FuseResultsSymmetricRanksTieToFirstSeen) {
    auto svc = MakeFusion();
    std::vector<std::vector<ScoredResult>> per_variant = {
        {{"a", 0.9f}, {"b", 0.8f}},  // a=rank1, b=rank2
        {{"b", 0.9f}, {"a", 0.7f}},  // b=rank1, a=rank2
    };
    auto out = svc->FuseResults(per_variant, 60);
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value().size(), 2u);
    const double tie = 1.0 / 61.0 + 1.0 / 62.0;
    EXPECT_NEAR(out.value()[0].score, tie, 1e-6);
    EXPECT_NEAR(out.value()[1].score, tie, 1e-6);
    EXPECT_EQ(out.value()[0].child_id, "a");  // a first-seen at position 0
    EXPECT_EQ(out.value()[1].child_id, "b");
}

// Non-tie path: distinct scores sort strictly by RRF descending (so the tiebreak
// only matters for equal scores).
TEST(QueryTiebreakTest, FuseResultsDistinctScoresSortByScore) {
    auto svc = MakeFusion();
    std::vector<std::vector<ScoredResult>> per_variant = {
        {{"top", 0.9f}, {"mid", 0.8f}, {"low", 0.7f}},
    };
    auto out = svc->FuseResults(per_variant, 60);
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value().size(), 3u);
    EXPECT_EQ(out.value()[0].child_id, "top");  // rank1 1/61
    EXPECT_EQ(out.value()[1].child_id, "mid");  // rank2 1/62
    EXPECT_EQ(out.value()[2].child_id, "low");  // rank3 1/63
    EXPECT_GT(out.value()[0].score, out.value()[1].score);
    EXPECT_GT(out.value()[1].score, out.value()[2].score);
}

// --- frozen cortrix::RRFFusion (block_id keyspace) tie invariants ------------

// Build a single-route RouteResult helper (vector route).
RouteResult VectorRoute(const std::vector<std::pair<uint64_t, float>>& items) {
    RouteResult r;
    r.route_name = "vector";
    r.status = RouteStatus::kSuccess;
    for (const auto& [id, score] : items) {
        RouteResultItem it;
        it.block_id = id;
        it.raw_score = score;
        r.items.push_back(it);
    }
    return r;
}

// Two routes that each place a distinct block at rank-1 => equal rrf_score. The
// frozen RRFFusion guarantees (a) every input block is present exactly once
// (dedup) and (b) tied blocks all carry the identical rrf_score; the relative
// order among ties is a stable_sort over a hash-map dump (not child_id-ordered),
// so we assert the score-level invariant, not a positional one.
TEST(QueryTiebreakTest, RrfFusionEqualScoreBlocksAllPresentWithSameScore) {
    RRFFusion fusion(60);
    std::vector<RouteResult> routes = {
        VectorRoute({{100, 0.9f}}),  // block 100 at rank 1
        VectorRoute({{200, 0.9f}}),  // block 200 at rank 1 (different route call)
    };
    // NOTE: Merge only sums across DISTINCT routes by route_name; both here are
    // "vector", so block 100 and 200 each appear once at rank 1 within their list.
    auto merged = fusion.Merge(routes);
    ASSERT_EQ(merged.size(), 2u);
    EXPECT_NEAR(merged[0].rrf_score, merged[1].rrf_score, 1e-6);  // tied
    std::vector<uint64_t> ids = {merged[0].block_id, merged[1].block_id};
    EXPECT_NE(std::find(ids.begin(), ids.end(), 100u), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), 200u), ids.end());
}

// Higher rank => strictly higher rrf_score (the ordering the tiebreak sits beneath).
TEST(QueryTiebreakTest, RrfFusionRankOrderingIsStrict) {
    RRFFusion fusion(60);
    auto merged = fusion.Merge({VectorRoute({{10, 0.9f}, {20, 0.8f}, {30, 0.7f}})});
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(merged[0].block_id, 10u);  // rank1 = 1/61 highest
    EXPECT_EQ(merged[1].block_id, 20u);  // rank2 = 1/62
    EXPECT_EQ(merged[2].block_id, 30u);  // rank3 = 1/63
    EXPECT_GT(merged[0].rrf_score, merged[1].rrf_score);
    EXPECT_GT(merged[1].rrf_score, merged[2].rrf_score);
}

// Dedup keeps the highest rrf_score for a repeated block and OR-merges hit_routes.
TEST(QueryTiebreakTest, RrfFusionDedupKeepsMaxAndMergesRoutes) {
    std::vector<ScoredBlock> blocks;
    ScoredBlock a;
    a.block_id = 7;
    a.rrf_score = 0.2f;
    a.hit_routes = kRouteVector;
    ScoredBlock b;
    b.block_id = 7;
    b.rrf_score = 0.5f;  // higher score wins
    b.hit_routes = kRouteBM25;
    blocks.push_back(a);
    blocks.push_back(b);
    auto out = RRFFusion::Dedup(blocks);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].block_id, 7u);
    EXPECT_FLOAT_EQ(out[0].rrf_score, 0.5f);
    EXPECT_EQ(out[0].hit_routes, kRouteVector | kRouteBM25);
}

}  // namespace
}  // namespace cortrix::query
