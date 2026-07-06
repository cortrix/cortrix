#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/query/rag_fusion.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/route_result.h"
#include "cortrix/query/scored_block.h"
#include "cortrix/retrieval/sparse_rrf.h"
#include "cortrix/retrieval/types.h"

// Value-matrix breadth coverage for the three RRF fusion surfaces.
//
// Verified real behavior:
//  A) cortrix::RRFFusion (query/rrf_fusion.cpp):
//       default k=60; contribution = 1/(k + rank) with rank = i+1 (1-BASED).
//       Merge keys by int64 block_id via unordered_map -> tie ORDER among equal
//       rrf_score is NOT deterministic (only scores/membership asserted on ties).
//       Truncate: top_k<=0 -> {}; else min(top_k, size).
//  B) cortrix::retrieval::FuseFivePathRrf (retrieval/sparse_rrf.cpp):
//       kRrfKDefault=60; k<=0 -> default; rank is 0-BASED (top hit = 1/k);
//       top_n<=0 -> all; DETERMINISTIC tie-break by child_id ASC;
//       contributing_paths sorted in enum order.
//  C) cortrix::query::RagFusion::FuseResults (query/rag_fusion.cpp):
//       default rrf_k=60; rrf_k<=0 -> 60; rank = r+1 (1-BASED);
//       tie-break by first_seen (insertion order across variants). FuseResults
//       never touches the generator, so a null generator is fine to construct.
//
// All suite + fixture names globally unique (RrfValMatrix* prefix).

namespace {

// ---- builders ------------------------------------------------------------

cortrix::RouteResult MakeRoute(const std::string& name,
                               std::vector<std::pair<uint64_t, float>> items,
                               cortrix::RouteStatus status =
                                   cortrix::RouteStatus::kSuccess) {
    cortrix::RouteResult rr;
    rr.route_name = name;
    rr.status = status;
    for (auto& [bid, score] : items) {
        cortrix::RouteResultItem it;
        it.block_id = bid;
        it.doc_id = "d" + std::to_string(bid);
        it.chunk_index = 0;
        it.raw_score = score;
        rr.items.push_back(it);
    }
    return rr;
}

cortrix::retrieval::SparseHit Hit(const std::string& id, float score) {
    cortrix::retrieval::SparseHit h;
    h.child_id = id;
    h.score = score;
    return h;
}

cortrix::retrieval::ScoredResult SR(const std::string& id, float score) {
    cortrix::retrieval::ScoredResult r;
    r.child_id = id;
    r.score = score;
    return r;
}

float FindScore(const std::vector<cortrix::ScoredBlock>& v, uint64_t id) {
    for (const auto& b : v)
        if (b.block_id == id) return b.rrf_score;
    return -1.0f;
}

// ==========================================================================
// A1. RRFFusion default k constant.
// ==========================================================================
TEST(RrfValMatrixRRFFusionDefaults, SingleRouteSingleItemUsesK60) {
    cortrix::RRFFusion fusion;  // default k=60
    cortrix::RouteResult r = MakeRoute("vector", {{1, 0.9f}});
    std::vector<cortrix::ScoredBlock> out = fusion.Merge({r});
    ASSERT_EQ(out.size(), 1u);
    // rank=1 -> 1/(60+1).
    EXPECT_FLOAT_EQ(out[0].rrf_score, 1.0f / 61.0f);
}

// ==========================================================================
// A2. RRFFusion k-parameter matrix: contribution = 1/(k + rank), rank 1-based.
// ==========================================================================
struct RrfValMatrixKCase {
    int k;
    int rank;  // 1-based position
};

class RrfValMatrixRRFFusionK
    : public ::testing::TestWithParam<RrfValMatrixKCase> {};

TEST_P(RrfValMatrixRRFFusionK, ContributionFormula) {
    const auto& tc = GetParam();
    cortrix::RRFFusion fusion(tc.k);
    // Build a route with `rank` items; the item at position rank has block_id 999.
    std::vector<std::pair<uint64_t, float>> items;
    for (int i = 1; i <= tc.rank; ++i) {
        items.push_back({static_cast<uint64_t>(i == tc.rank ? 999 : i),
                         1.0f / static_cast<float>(i)});
    }
    cortrix::RouteResult r = MakeRoute("vector", items);
    std::vector<cortrix::ScoredBlock> out = fusion.Merge({r});
    float got = FindScore(out, 999);
    ASSERT_GE(got, 0.0f);
    EXPECT_FLOAT_EQ(got, 1.0f / static_cast<float>(tc.k + tc.rank));
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, RrfValMatrixRRFFusionK,
    ::testing::Values(
        RrfValMatrixKCase{60, 1}, RrfValMatrixKCase{60, 2},
        RrfValMatrixKCase{60, 10}, RrfValMatrixKCase{1, 1},
        RrfValMatrixKCase{1, 5}, RrfValMatrixKCase{10, 3},
        RrfValMatrixKCase{100, 1}, RrfValMatrixKCase{1000, 4}));

// ==========================================================================
// A3. RRFFusion multi-route summation + hit_routes bitmask.
// ==========================================================================
TEST(RrfValMatrixRRFFusionMulti, BlockInTwoRoutesSumsContributions) {
    cortrix::RRFFusion fusion(60);
    // block 7 is rank 1 in vector and rank 2 in bm25.
    cortrix::RouteResult v = MakeRoute("vector", {{7, 0.9f}});
    cortrix::RouteResult b = MakeRoute("bm25", {{5, 0.8f}, {7, 0.7f}});
    std::vector<cortrix::ScoredBlock> out = fusion.Merge({v, b});
    float s7 = FindScore(out, 7);
    EXPECT_FLOAT_EQ(s7, 1.0f / 61.0f + 1.0f / 62.0f);
    // hit_routes should carry both vector + bm25 bits.
    for (const auto& blk : out) {
        if (blk.block_id == 7) {
            EXPECT_EQ(blk.hit_routes,
                      static_cast<uint8_t>(cortrix::kRouteVector |
                                           cortrix::kRouteBM25));
            EXPECT_EQ(blk.hit_count(), 2);
        }
    }
}

// Non-success routes are skipped entirely.
class RrfValMatrixRRFFusionStatus
    : public ::testing::TestWithParam<cortrix::RouteStatus> {};

TEST_P(RrfValMatrixRRFFusionStatus, NonSuccessRouteIgnored) {
    cortrix::RRFFusion fusion(60);
    cortrix::RouteResult bad = MakeRoute("vector", {{1, 0.9f}}, GetParam());
    std::vector<cortrix::ScoredBlock> out = fusion.Merge({bad});
    EXPECT_TRUE(out.empty());
}

INSTANTIATE_TEST_SUITE_P(Matrix, RrfValMatrixRRFFusionStatus,
                         ::testing::Values(cortrix::RouteStatus::kTimeout,
                                           cortrix::RouteStatus::kError,
                                           cortrix::RouteStatus::kSkipped));

// Empty routes / empty items -> empty result.
TEST(RrfValMatrixRRFFusionMulti, EmptyInputs) {
    cortrix::RRFFusion fusion(60);
    EXPECT_TRUE(fusion.Merge({}).empty());
    EXPECT_TRUE(fusion.Merge({MakeRoute("vector", {})}).empty());
}

// Descending order by rrf_score (distinct scores -> deterministic order).
TEST(RrfValMatrixRRFFusionMulti, SortedDescendingDistinctScores) {
    cortrix::RRFFusion fusion(60);
    cortrix::RouteResult v = MakeRoute("vector", {{1, 0.9f}, {2, 0.8f}, {3, 0.7f}});
    std::vector<cortrix::ScoredBlock> out = fusion.Merge({v});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_GE(out[0].rrf_score, out[1].rrf_score);
    EXPECT_GE(out[1].rrf_score, out[2].rrf_score);
    EXPECT_EQ(out[0].block_id, 1u);  // rank 1 = highest contribution
}

// ==========================================================================
// A4. RRFFusion::Truncate top_k matrix.
// ==========================================================================
struct RrfValMatrixTruncCase {
    int top_k;
    size_t expect;  // out of 4
};

class RrfValMatrixTruncate
    : public ::testing::TestWithParam<RrfValMatrixTruncCase> {};

TEST_P(RrfValMatrixTruncate, TruncBoundaries) {
    const auto& tc = GetParam();
    std::vector<cortrix::ScoredBlock> blocks(4);
    for (int i = 0; i < 4; ++i) {
        blocks[i].block_id = static_cast<uint64_t>(i);
        blocks[i].rrf_score = 1.0f - 0.1f * static_cast<float>(i);
    }
    std::vector<cortrix::ScoredBlock> out =
        cortrix::RRFFusion::Truncate(blocks, tc.top_k);
    EXPECT_EQ(out.size(), tc.expect);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, RrfValMatrixTruncate,
    ::testing::Values(RrfValMatrixTruncCase{0, 0}, RrfValMatrixTruncCase{-1, 0},
                      RrfValMatrixTruncCase{-100, 0}, RrfValMatrixTruncCase{1, 1},
                      RrfValMatrixTruncCase{4, 4}, RrfValMatrixTruncCase{5, 4},
                      RrfValMatrixTruncCase{99999, 4}));

// ==========================================================================
// A5. RRFFusion::Dedup keep-max.
// ==========================================================================
TEST(RrfValMatrixRRFFusionDedup, KeepsMaxScoreMergesRoutes) {
    std::vector<cortrix::ScoredBlock> blocks(3);
    blocks[0].block_id = 1;
    blocks[0].rrf_score = 0.3f;
    blocks[0].hit_routes = cortrix::kRouteVector;
    blocks[1].block_id = 1;
    blocks[1].rrf_score = 0.7f;  // higher
    blocks[1].hit_routes = cortrix::kRouteBM25;
    blocks[2].block_id = 2;
    blocks[2].rrf_score = 0.5f;
    std::vector<cortrix::ScoredBlock> out = cortrix::RRFFusion::Dedup(blocks);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_FLOAT_EQ(FindScore(out, 1), 0.7f);
    for (const auto& b : out) {
        if (b.block_id == 1) {
            EXPECT_EQ(b.hit_routes,
                      static_cast<uint8_t>(cortrix::kRouteVector |
                                           cortrix::kRouteBM25));
        }
    }
}

// ==========================================================================
// B1. FuseFivePathRrf k-param matrix: rank 0-BASED, contribution = 1/(k+rank).
// ==========================================================================
struct RrfValMatrixFiveKCase {
    int k_in;
    int k_eff;  // effective k after k<=0 -> default guard
};

class RrfValMatrixFivePathK
    : public ::testing::TestWithParam<RrfValMatrixFiveKCase> {};

TEST_P(RrfValMatrixFivePathK, TopHitContribIsOneOverK) {
    const auto& tc = GetParam();
    cortrix::retrieval::FivePathInput in;
    in.dense = {Hit("AAA", 0.9f)};  // rank 0 -> 1/k_eff
    std::vector<cortrix::retrieval::RrfFusedHit> out =
        cortrix::retrieval::FuseFivePathRrf(in, /*top_n=*/0, tc.k_in);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].rrf_score, 1.0f / static_cast<float>(tc.k_eff));
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, RrfValMatrixFivePathK,
    ::testing::Values(
        RrfValMatrixFiveKCase{60, 60},  // explicit default
        RrfValMatrixFiveKCase{0, 60},   // k<=0 -> default 60
        RrfValMatrixFiveKCase{-1, 60},
        RrfValMatrixFiveKCase{-100, 60},
        RrfValMatrixFiveKCase{1, 1},
        RrfValMatrixFiveKCase{10, 10},
        RrfValMatrixFiveKCase{1000, 1000}));

TEST(RrfValMatrixFivePathK, DefaultConstantIs60) {
    EXPECT_EQ(cortrix::retrieval::kRrfKDefault, 60);
    EXPECT_EQ(cortrix::retrieval::kRrfPathCount, 5);
}

// B2. rank progression within a path: 1/(k+0), 1/(k+1), 1/(k+2)...
TEST(RrfValMatrixFivePathRank, RankProgressionZeroBased) {
    cortrix::retrieval::FivePathInput in;
    in.dense = {Hit("A", 0), Hit("B", 0), Hit("C", 0)};
    std::vector<cortrix::retrieval::RrfFusedHit> out =
        cortrix::retrieval::FuseFivePathRrf(in, 0, 60);
    ASSERT_EQ(out.size(), 3u);
    // Deterministic order: A(1/60) > B(1/61) > C(1/62).
    EXPECT_EQ(out[0].child_id, "A");
    EXPECT_FLOAT_EQ(out[0].rrf_score, 1.0f / 60.0f);
    EXPECT_EQ(out[1].child_id, "B");
    EXPECT_FLOAT_EQ(out[1].rrf_score, 1.0f / 61.0f);
    EXPECT_EQ(out[2].child_id, "C");
    EXPECT_FLOAT_EQ(out[2].rrf_score, 1.0f / 62.0f);
}

// B3. Cross-path summation + contributing_paths in enum order.
TEST(RrfValMatrixFivePathPaths, SumsAcrossPathsAndLabelsInOrder) {
    cortrix::retrieval::FivePathInput in;
    in.dense = {Hit("X", 0)};   // rank0 dense
    in.sparse = {Hit("X", 0)};  // rank0 sparse
    in.fts5 = {Hit("X", 0)};    // rank0 fts5
    std::vector<cortrix::retrieval::RrfFusedHit> out =
        cortrix::retrieval::FuseFivePathRrf(in, 0, 60);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].rrf_score, 3.0f / 60.0f);
    ASSERT_EQ(out[0].contributing_paths.size(), 3u);
    // enum order: kDense(0) < kSparse(2) < kFts5(3).
    EXPECT_EQ(out[0].contributing_paths[0], cortrix::retrieval::RrfPath::kDense);
    EXPECT_EQ(out[0].contributing_paths[1], cortrix::retrieval::RrfPath::kSparse);
    EXPECT_EQ(out[0].contributing_paths[2], cortrix::retrieval::RrfPath::kFts5);
}

// B4. Deterministic tie-break by child_id ASC.
TEST(RrfValMatrixFivePathTie, TieBreakByChildIdAscending) {
    cortrix::retrieval::FivePathInput in;
    // RRF rank = POSITION within a path list (the Hit score field is unused for
    // ranking). To create a genuine score tie, place each id at rank 0 (position 0)
    // in a DIFFERENT path -> identical score 1/60 -> deterministic child_id ASC.
    in.dense = {Hit("ccc", 0.9f)};
    in.sparse = {Hit("aaa", 0.9f)};
    in.fts5 = {Hit("bbb", 0.9f)};
    std::vector<cortrix::retrieval::RrfFusedHit> out =
        cortrix::retrieval::FuseFivePathRrf(in, 0, 60);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].child_id, "aaa");
    EXPECT_EQ(out[1].child_id, "bbb");
    EXPECT_EQ(out[2].child_id, "ccc");
}

// B5. top_n boundary matrix (<=0 -> all).
struct RrfValMatrixFiveTopNCase {
    int top_n;
    size_t expect;  // out of 4 distinct-score hits
};

class RrfValMatrixFivePathTopN
    : public ::testing::TestWithParam<RrfValMatrixFiveTopNCase> {};

TEST_P(RrfValMatrixFivePathTopN, TopNBoundaries) {
    const auto& tc = GetParam();
    cortrix::retrieval::FivePathInput in;
    in.dense = {Hit("A", 0), Hit("B", 0), Hit("C", 0), Hit("D", 0)};
    std::vector<cortrix::retrieval::RrfFusedHit> out =
        cortrix::retrieval::FuseFivePathRrf(in, tc.top_n, 60);
    EXPECT_EQ(out.size(), tc.expect);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, RrfValMatrixFivePathTopN,
    ::testing::Values(RrfValMatrixFiveTopNCase{0, 4},   // <=0 -> all
                      RrfValMatrixFiveTopNCase{-1, 4},  // <=0 -> all
                      RrfValMatrixFiveTopNCase{1, 1}, RrfValMatrixFiveTopNCase{2, 2},
                      RrfValMatrixFiveTopNCase{4, 4}, RrfValMatrixFiveTopNCase{99, 4}));

// B6. All-empty input -> empty.
TEST(RrfValMatrixFivePathTopN, AllEmptyInput) {
    cortrix::retrieval::FivePathInput in;
    std::vector<cortrix::retrieval::RrfFusedHit> out =
        cortrix::retrieval::FuseFivePathRrf(in, 0, 60);
    EXPECT_TRUE(out.empty());
}

// ==========================================================================
// C. RagFusion::FuseResults — global second-pass RRF (rank 1-based).
// FuseResults never dereferences the generator, so null is safe.
// ==========================================================================
cortrix::query::RagFusion MakeRagFusion() {
    auto rrf = std::make_shared<cortrix::RRFFusion>(60);
    return cortrix::query::RagFusion(/*generator=*/nullptr, rrf);
}

struct RrfValMatrixFuseKCase {
    int rrf_k_in;
    int rrf_k_eff;
};

class RrfValMatrixFuseResultsK
    : public ::testing::TestWithParam<RrfValMatrixFuseKCase> {};

TEST_P(RrfValMatrixFuseResultsK, RankOneBasedContribution) {
    const auto& tc = GetParam();
    cortrix::query::RagFusion rf = MakeRagFusion();
    // One list means the original query role. F36 v1.0.7 anchored weighted RRF
    // makes its rank-1 contribution 1.7/(k+1); anchoring changes order, not score.
    std::vector<std::vector<cortrix::retrieval::ScoredResult>> per_variant = {
        {SR("ID1", 0.9f)}};
    cortrix::Result<std::vector<cortrix::retrieval::ScoredResult>> res =
        rf.FuseResults(per_variant, tc.rrf_k_in);
    ASSERT_TRUE(res.ok());
    const std::vector<cortrix::retrieval::ScoredResult>& out = res.value();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].score,
                    static_cast<float>(1.7 / (tc.rrf_k_eff + 1)));
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, RrfValMatrixFuseResultsK,
    ::testing::Values(RrfValMatrixFuseKCase{60, 60}, RrfValMatrixFuseKCase{0, 60},
                      RrfValMatrixFuseKCase{-1, 60}, RrfValMatrixFuseKCase{-50, 60},
                      RrfValMatrixFuseKCase{1, 1}, RrfValMatrixFuseKCase{10, 10},
                      RrfValMatrixFuseKCase{500, 500}));

// C2. Cross-variant summation (same ChildId in two variants).
TEST(RrfValMatrixFuseResults, SumsAcrossVariants) {
    cortrix::query::RagFusion rf = MakeRagFusion();
    std::vector<std::vector<cortrix::retrieval::ScoredResult>> per_variant = {
        {SR("DUP", 0.9f)},                    // original, rank1
        {SR("OTHER", 0.5f), SR("DUP", 0.4f)}  // variant: OTHER rank1, DUP rank2
    };
    cortrix::Result<std::vector<cortrix::retrieval::ScoredResult>> res =
        rf.FuseResults(per_variant, 60);
    ASSERT_TRUE(res.ok());
    const std::vector<cortrix::retrieval::ScoredResult>& out = res.value();
    ASSERT_EQ(out.size(), 2u);
    // DUP = 1.7/61 + 0.5/62, the largest.
    EXPECT_EQ(out[0].child_id, "DUP");
    EXPECT_FLOAT_EQ(out[0].score,
                    static_cast<float>(1.7 / 61.0 + 0.5 / 62.0));
    EXPECT_EQ(out[1].child_id, "OTHER");
    EXPECT_FLOAT_EQ(out[1].score, static_cast<float>(0.5 / 61.0));
}

// C3. Tie-break by first_seen (insertion order across variants).
TEST(RrfValMatrixFuseResults, TieBreakByFirstSeenInsertionOrder) {
    cortrix::query::RagFusion rf = MakeRagFusion();
    // Empty original + all variant-only rank-1 hits -> identical weighted RRF score
    // -> stable by first-seen insertion order (A then B then C).
    std::vector<std::vector<cortrix::retrieval::ScoredResult>> per_variant = {
        {}, {SR("A", 0.1f)}, {SR("B", 0.1f)}, {SR("C", 0.1f)}};
    cortrix::Result<std::vector<cortrix::retrieval::ScoredResult>> res =
        rf.FuseResults(per_variant, 60);
    ASSERT_TRUE(res.ok());
    const std::vector<cortrix::retrieval::ScoredResult>& out = res.value();
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].child_id, "A");
    EXPECT_EQ(out[1].child_id, "B");
    EXPECT_EQ(out[2].child_id, "C");
}

// C4. Empty input -> empty (Ok).
TEST(RrfValMatrixFuseResults, EmptyInputOkEmpty) {
    cortrix::query::RagFusion rf = MakeRagFusion();
    std::vector<std::vector<cortrix::retrieval::ScoredResult>> empty;
    cortrix::Result<std::vector<cortrix::retrieval::ScoredResult>> res =
        rf.FuseResults(empty, 60);
    ASSERT_TRUE(res.ok());
    EXPECT_TRUE(res.value().empty());
}

// C5. Default rrf_k param value is 60 (signature default).
TEST(RrfValMatrixFuseResults, DefaultRrfKParamIs60) {
    cortrix::query::RagFusion rf = MakeRagFusion();
    std::vector<std::vector<cortrix::retrieval::ScoredResult>> per_variant = {
        {SR("Z", 0.9f)}};
    // Call WITHOUT rrf_k -> uses default 60. Single list is original role -> 1.7/61.
    cortrix::Result<std::vector<cortrix::retrieval::ScoredResult>> res =
        rf.FuseResults(per_variant);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res.value().size(), 1u);
    EXPECT_FLOAT_EQ(res.value()[0].score, static_cast<float>(1.7 / 61.0));
}

}  // namespace
