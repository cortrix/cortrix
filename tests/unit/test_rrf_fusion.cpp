#include <gtest/gtest.h>
#include "cortrix/query/rrf_fusion.h"

namespace cortrix {
namespace {

// Helper to create a RouteResult with items
RouteResult MakeRoute(const std::string& name,
                       std::vector<std::pair<int64_t, float>> block_scores) {
    RouteResult rr;
    rr.route_name = name;
    rr.status = RouteStatus::kSuccess;
    for (auto& [bid, score] : block_scores) {
        RouteResultItem item;
        item.block_id = bid;
        item.doc_id = bid * 10;
        item.chunk_index = 0;
        item.raw_score = score;
        rr.items.push_back(item);
    }
    return rr;
}

// --- Merge tests ---

TEST(RRFFusionTest, MergeTwoRoutes_HandCalculated) {
    // Example from design doc:
    // Vector: blk_1(0.95), blk_2(0.87), blk_3(0.72)
    // BM25:   blk_2(12.3), blk_4(9.1),  blk_1(7.5)
    // k=60
    RRFFusion fusion(60);

    auto vec = MakeRoute("vector", {{1, 0.95f}, {2, 0.87f}, {3, 0.72f}});
    auto bm25 = MakeRoute("bm25", {{2, 12.3f}, {4, 9.1f}, {1, 7.5f}});

    auto result = fusion.Merge({vec, bm25});

    ASSERT_EQ(result.size(), 4u);

    // blk_2: vector rank=2 (1/62) + bm25 rank=1 (1/61) = 0.01613 + 0.01639 = 0.03252
    // blk_1: vector rank=1 (1/61) + bm25 rank=3 (1/63) = 0.01639 + 0.01587 = 0.03226
    // blk_4: bm25 rank=2 (1/62) = 0.01613
    // blk_3: vector rank=3 (1/63) = 0.01587

    // Result sorted by rrf_score descending
    EXPECT_EQ(result[0].block_id, 2);
    EXPECT_NEAR(result[0].rrf_score, 1.0f/62 + 1.0f/61, 1e-5);
    EXPECT_EQ(result[0].hit_routes, kRouteVector | kRouteBM25);

    EXPECT_EQ(result[1].block_id, 1);
    EXPECT_NEAR(result[1].rrf_score, 1.0f/61 + 1.0f/63, 1e-5);
    EXPECT_EQ(result[1].hit_routes, kRouteVector | kRouteBM25);

    EXPECT_EQ(result[2].block_id, 4);
    EXPECT_NEAR(result[2].rrf_score, 1.0f/62, 1e-5);
    EXPECT_EQ(result[2].hit_routes, kRouteBM25);

    EXPECT_EQ(result[3].block_id, 3);
    EXPECT_NEAR(result[3].rrf_score, 1.0f/63, 1e-5);
    EXPECT_EQ(result[3].hit_routes, kRouteVector);
}

TEST(RRFFusionTest, MergeSingleRoute) {
    RRFFusion fusion(60);
    auto vec = MakeRoute("vector", {{10, 0.9f}, {20, 0.8f}});

    auto result = fusion.Merge({vec});

    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].block_id, 10);
    EXPECT_NEAR(result[0].rrf_score, 1.0f/61, 1e-5);
    EXPECT_EQ(result[1].block_id, 20);
    EXPECT_NEAR(result[1].rrf_score, 1.0f/62, 1e-5);
}

TEST(RRFFusionTest, MergeEmptyRoutes) {
    RRFFusion fusion(60);
    auto result = fusion.Merge({});
    EXPECT_TRUE(result.empty());
}

TEST(RRFFusionTest, MergeSkipsFailedRoutes) {
    RRFFusion fusion(60);
    auto vec = MakeRoute("vector", {{1, 0.9f}});

    RouteResult failed;
    failed.route_name = "bm25";
    failed.status = RouteStatus::kError;

    auto result = fusion.Merge({vec, failed});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].block_id, 1);
}

TEST(RRFFusionTest, MergeWithEmptyResults) {
    RRFFusion fusion(60);
    auto vec = MakeRoute("vector", {});
    vec.status = RouteStatus::kSuccess;

    auto bm25 = MakeRoute("bm25", {{1, 5.0f}});

    auto result = fusion.Merge({vec, bm25});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].block_id, 1);
}

TEST(RRFFusionTest, MergeCustomK) {
    RRFFusion fusion(10);  // k=10
    auto vec = MakeRoute("vector", {{1, 0.9f}});

    auto result = fusion.Merge({vec});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_NEAR(result[0].rrf_score, 1.0f/11, 1e-5);
}

// --- Dedup tests ---

TEST(RRFFusionTest, DedupMergesSameBlockId) {
    std::vector<ScoredBlock> blocks;

    ScoredBlock sb1;
    sb1.block_id = 1;
    sb1.rrf_score = 0.5f;
    sb1.hit_routes = kRouteVector;
    blocks.push_back(sb1);

    ScoredBlock sb2;
    sb2.block_id = 1;
    sb2.rrf_score = 0.3f;
    sb2.hit_routes = kRouteBM25;
    blocks.push_back(sb2);

    auto result = RRFFusion::Dedup(blocks);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].block_id, 1);
    EXPECT_FLOAT_EQ(result[0].rrf_score, 0.5f);  // keeps highest score
    EXPECT_EQ(result[0].hit_routes, kRouteVector | kRouteBM25);
}

TEST(RRFFusionTest, DedupNoDeduplication) {
    std::vector<ScoredBlock> blocks;

    ScoredBlock sb1;
    sb1.block_id = 1;
    sb1.rrf_score = 0.5f;
    blocks.push_back(sb1);

    ScoredBlock sb2;
    sb2.block_id = 2;
    sb2.rrf_score = 0.3f;
    blocks.push_back(sb2);

    auto result = RRFFusion::Dedup(blocks);
    ASSERT_EQ(result.size(), 2u);
}

// --- Truncate tests ---

TEST(RRFFusionTest, TruncateToTopK) {
    std::vector<ScoredBlock> blocks(5);
    for (int i = 0; i < 5; ++i) {
        blocks[i].block_id = i + 1;
        blocks[i].rrf_score = 1.0f / (i + 1);
    }

    auto result = RRFFusion::Truncate(blocks, 2);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].block_id, 1);
    EXPECT_EQ(result[1].block_id, 2);
}

TEST(RRFFusionTest, TruncateLargerThanInput) {
    std::vector<ScoredBlock> blocks(2);
    blocks[0].block_id = 1;
    blocks[1].block_id = 2;

    auto result = RRFFusion::Truncate(blocks, 10);
    ASSERT_EQ(result.size(), 2u);
}

TEST(RRFFusionTest, TruncateZero) {
    std::vector<ScoredBlock> blocks(3);
    auto result = RRFFusion::Truncate(blocks, 0);
    EXPECT_TRUE(result.empty());
}

TEST(RRFFusionTest, TruncateEmpty) {
    std::vector<ScoredBlock> blocks;
    auto result = RRFFusion::Truncate(blocks, 5);
    EXPECT_TRUE(result.empty());
}

TEST(RRFFusionTest, TruncateNegative) {
    std::vector<ScoredBlock> blocks(3);
    auto result = RRFFusion::Truncate(blocks, -1);
    EXPECT_TRUE(result.empty());
}

TEST(RRFFusionTest, TruncateExactMatch) {
    std::vector<ScoredBlock> blocks(3);
    blocks[0].block_id = 1;
    blocks[1].block_id = 2;
    blocks[2].block_id = 3;
    auto result = RRFFusion::Truncate(blocks, 3);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[2].block_id, 3);
}

// --- Additional Dedup tests ---

TEST(RRFFusionTest, DedupEmpty) {
    std::vector<ScoredBlock> blocks;
    auto result = RRFFusion::Dedup(blocks);
    EXPECT_TRUE(result.empty());
}

TEST(RRFFusionTest, DedupThreeDuplicates_KeepsHighestScore) {
    std::vector<ScoredBlock> blocks;

    ScoredBlock sb1;
    sb1.block_id = 1;
    sb1.rrf_score = 0.2f;
    sb1.hit_routes = kRouteVector;
    blocks.push_back(sb1);

    ScoredBlock sb2;
    sb2.block_id = 1;
    sb2.rrf_score = 0.5f;
    sb2.hit_routes = kRouteBM25;
    blocks.push_back(sb2);

    ScoredBlock sb3;
    sb3.block_id = 1;
    sb3.rrf_score = 0.3f;
    sb3.hit_routes = kRouteSQL;
    blocks.push_back(sb3);

    auto result = RRFFusion::Dedup(blocks);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].block_id, 1);
    EXPECT_FLOAT_EQ(result[0].rrf_score, 0.5f);
    EXPECT_EQ(result[0].hit_routes, kRouteVector | kRouteBM25 | kRouteSQL);
}

TEST(RRFFusionTest, DedupMixedUniqueAndDuplicates) {
    std::vector<ScoredBlock> blocks;

    ScoredBlock sb1;
    sb1.block_id = 1;
    sb1.rrf_score = 0.9f;
    sb1.hit_routes = kRouteVector;
    blocks.push_back(sb1);

    ScoredBlock sb2;
    sb2.block_id = 2;
    sb2.rrf_score = 0.8f;
    sb2.hit_routes = kRouteBM25;
    blocks.push_back(sb2);

    ScoredBlock sb3;
    sb3.block_id = 1;
    sb3.rrf_score = 0.7f;
    sb3.hit_routes = kRouteBM25;
    blocks.push_back(sb3);

    ScoredBlock sb4;
    sb4.block_id = 3;
    sb4.rrf_score = 0.6f;
    sb4.hit_routes = kRouteSQL;
    blocks.push_back(sb4);

    auto result = RRFFusion::Dedup(blocks);
    ASSERT_EQ(result.size(), 3u);
    // Sorted by rrf_score descending: block 1 (0.9), block 2 (0.8), block 3 (0.6)
    EXPECT_EQ(result[0].block_id, 1);
    EXPECT_FLOAT_EQ(result[0].rrf_score, 0.9f);
    EXPECT_EQ(result[0].hit_routes, kRouteVector | kRouteBM25);  // merged routes
    EXPECT_EQ(result[1].block_id, 2);
    EXPECT_EQ(result[2].block_id, 3);
}

TEST(RRFFusionTest, DedupPreservesDescendingOrder) {
    std::vector<ScoredBlock> blocks;

    ScoredBlock sb1;
    sb1.block_id = 1;
    sb1.rrf_score = 0.1f;
    blocks.push_back(sb1);

    ScoredBlock sb2;
    sb2.block_id = 2;
    sb2.rrf_score = 0.9f;
    blocks.push_back(sb2);

    ScoredBlock sb3;
    sb3.block_id = 3;
    sb3.rrf_score = 0.5f;
    blocks.push_back(sb3);

    auto result = RRFFusion::Dedup(blocks);
    ASSERT_EQ(result.size(), 3u);
    // Should be sorted: 0.9, 0.5, 0.1
    EXPECT_FLOAT_EQ(result[0].rrf_score, 0.9f);
    EXPECT_FLOAT_EQ(result[1].rrf_score, 0.5f);
    EXPECT_FLOAT_EQ(result[2].rrf_score, 0.1f);
}

// --- Merge additional edge cases ---

TEST(RRFFusionTest, MergeThreeRoutes) {
    RRFFusion fusion(60);

    auto vec = MakeRoute("vector", {{1, 0.95f}, {2, 0.87f}});
    auto bm25 = MakeRoute("bm25", {{2, 12.3f}, {3, 9.1f}});
    auto sql = MakeRoute("sql", {{3, 5.0f}, {1, 3.0f}});

    auto result = fusion.Merge({vec, bm25, sql});

    // Block 1: vector rank=1 (1/61) + sql rank=2 (1/62)
    // Block 2: vector rank=2 (1/62) + bm25 rank=1 (1/61)
    // Block 3: bm25 rank=2 (1/62) + sql rank=1 (1/61)
    ASSERT_EQ(result.size(), 3u);

    // All three blocks have scores from two routes each
    for (const auto& sb : result) {
        EXPECT_EQ(sb.hit_count(), 2);
    }
}

TEST(RRFFusionTest, MergeAllSkippedRoutes) {
    RRFFusion fusion(60);

    RouteResult skipped1{"vector", RouteStatus::kSkipped};
    RouteResult skipped2{"bm25", RouteStatus::kSkipped};

    auto result = fusion.Merge({skipped1, skipped2});
    EXPECT_TRUE(result.empty());
}

// --- ScoredBlock tests ---

TEST(ScoredBlockTest, HitCountZero) {
    ScoredBlock sb;
    sb.hit_routes = 0;
    EXPECT_EQ(sb.hit_count(), 0);
}

TEST(ScoredBlockTest, HitCountSingle) {
    ScoredBlock sb;
    sb.hit_routes = kRouteVector;
    EXPECT_EQ(sb.hit_count(), 1);
}

TEST(ScoredBlockTest, HitCountDouble) {
    ScoredBlock sb;
    sb.hit_routes = kRouteVector | kRouteBM25;
    EXPECT_EQ(sb.hit_count(), 2);
}

TEST(ScoredBlockTest, HitCountTriple) {
    ScoredBlock sb;
    sb.hit_routes = kRouteVector | kRouteBM25 | kRouteSQL;
    EXPECT_EQ(sb.hit_count(), 3);
}

TEST(ScoredBlockTest, CompareByScore) {
    ScoredBlock a, b;
    a.rrf_score = 0.5f;
    b.rrf_score = 0.3f;
    EXPECT_TRUE(a > b);
    EXPECT_FALSE(b > a);
}

// --- Merge: raw-score branches per route flag ---

// Build a RouteResult directly (the file-level MakeRoute helper assigns doc_id
// from an int; here we set the string doc_id explicitly).
RouteResult Route(const std::string& name, RouteStatus status,
                  std::vector<std::pair<uint64_t, float>> items) {
    RouteResult rr;
    rr.route_name = name;
    rr.status = status;
    for (auto& [bid, score] : items) {
        RouteResultItem it;
        it.block_id = bid;
        it.doc_id = "doc_" + std::to_string(bid);
        it.chunk_index = 0;
        it.raw_score = score;
        rr.items.push_back(it);
    }
    return rr;
}

// An unrecognized route name leaves route_flag == 0: the block is still merged
// (rrf contribution applied) but no route bit is set and neither raw-score arm runs.
TEST(RRFFusionTest, MergeUnknownRouteNameNoFlag) {
    RRFFusion fusion(60);
    auto unknown = Route("graph", RouteStatus::kSuccess, {{1, 0.5f}});
    auto result = fusion.Merge({unknown});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].hit_routes, 0u);          // no flag set
    EXPECT_FLOAT_EQ(result[0].vector_score, 0.0f);
    EXPECT_FLOAT_EQ(result[0].bm25_score, 0.0f);
    EXPECT_NEAR(result[0].rrf_score, 1.0f / 61, 1e-6);  // still contributes
}

// A SQL route writes the sql flag but NOT vector/bm25 raw scores (the else-if arms
// fall through), in both the new-block and existing-block merge paths.
TEST(RRFFusionTest, MergeSqlRouteSetsFlagButNoRawScores) {
    RRFFusion fusion(60);
    auto vec = Route("vector", RouteStatus::kSuccess, {{1, 0.9f}});
    auto sql = Route("sql", RouteStatus::kSuccess, {{1, 42.0f}, {2, 7.0f}});
    auto result = fusion.Merge({vec, sql});
    ASSERT_EQ(result.size(), 2u);
    for (const auto& sb : result) {
        if (sb.block_id == 1) {
            EXPECT_EQ(sb.hit_routes, kRouteVector | kRouteSQL);
            EXPECT_FLOAT_EQ(sb.vector_score, 0.9f);  // from vector route
            EXPECT_FLOAT_EQ(sb.bm25_score, 0.0f);    // sql never sets bm25
        } else {
            EXPECT_EQ(sb.hit_routes, kRouteSQL);     // sql-only new block
            EXPECT_FLOAT_EQ(sb.vector_score, 0.0f);
            EXPECT_FLOAT_EQ(sb.bm25_score, 0.0f);
        }
    }
}

// Dedup preserves non-zero vector_score / bm25_score from the incoming duplicate
// (the `!= 0.0f` true arms) and ORs hit_routes.
TEST(RRFFusionTest, DedupPreservesNonZeroRawScores) {
    std::vector<ScoredBlock> blocks;
    ScoredBlock a;  // first occurrence: only vector score
    a.block_id = 1; a.rrf_score = 0.2f; a.hit_routes = kRouteVector; a.vector_score = 0.8f;
    ScoredBlock b;  // duplicate: only bm25 score, lower rrf
    b.block_id = 1; b.rrf_score = 0.1f; b.hit_routes = kRouteBM25; b.bm25_score = 5.5f;
    blocks.push_back(a);
    blocks.push_back(b);

    auto merged = RRFFusion::Dedup(blocks);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_FLOAT_EQ(merged[0].rrf_score, 0.2f);             // higher kept
    EXPECT_EQ(merged[0].hit_routes, kRouteVector | kRouteBM25);  // ORed
    EXPECT_FLOAT_EQ(merged[0].vector_score, 0.8f);         // from a
    EXPECT_FLOAT_EQ(merged[0].bm25_score, 5.5f);           // from b (non-zero preserved)
}

// Dedup with the incoming duplicate carrying a HIGHER rrf_score replaces it (the
// `sb.rrf_score > it->second.rrf_score` true arm), and a zero incoming raw score
// does NOT overwrite an existing non-zero one (the `!= 0.0f` false arm).
TEST(RRFFusionTest, DedupHigherScoreReplacesZeroRawScoreDoesNot) {
    std::vector<ScoredBlock> blocks;
    ScoredBlock a;
    a.block_id = 7; a.rrf_score = 0.1f; a.hit_routes = kRouteVector; a.vector_score = 0.6f;
    ScoredBlock b;  // higher rrf, but vector_score 0 -> must not clobber a's 0.6
    b.block_id = 7; b.rrf_score = 0.4f; b.hit_routes = kRouteVector; b.vector_score = 0.0f;
    blocks.push_back(a);
    blocks.push_back(b);

    auto merged = RRFFusion::Dedup(blocks);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_FLOAT_EQ(merged[0].rrf_score, 0.4f);     // replaced with higher
    EXPECT_FLOAT_EQ(merged[0].vector_score, 0.6f);  // existing non-zero kept
}

}  // namespace
}  // namespace cortrix
