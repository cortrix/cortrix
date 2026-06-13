#include <gtest/gtest.h>
#include "cortrix/query/scored_block.h"

namespace cortrix {

// Test RouteFlag bitmask operations
TEST(ScoredBlockTest, HitCountZeroRoutes) {
    ScoredBlock sb;
    sb.hit_routes = 0;
    EXPECT_EQ(sb.hit_count(), 0);
}

TEST(ScoredBlockTest, HitCountSingleRoute) {
    ScoredBlock sb;
    sb.hit_routes = kRouteVector;
    EXPECT_EQ(sb.hit_count(), 1);

    sb.hit_routes = kRouteBM25;
    EXPECT_EQ(sb.hit_count(), 1);

    sb.hit_routes = kRouteSQL;
    EXPECT_EQ(sb.hit_count(), 1);
}

TEST(ScoredBlockTest, HitCountTwoRoutes) {
    ScoredBlock sb;
    sb.hit_routes = kRouteVector | kRouteBM25;
    EXPECT_EQ(sb.hit_count(), 2);

    sb.hit_routes = kRouteVector | kRouteSQL;
    EXPECT_EQ(sb.hit_count(), 2);

    sb.hit_routes = kRouteBM25 | kRouteSQL;
    EXPECT_EQ(sb.hit_count(), 2);
}

TEST(ScoredBlockTest, HitCountThreeRoutes) {
    ScoredBlock sb;
    sb.hit_routes = kRouteVector | kRouteBM25 | kRouteSQL;
    EXPECT_EQ(sb.hit_count(), 3);
}

// Test comparison operator
TEST(ScoredBlockTest, ComparisonOperator) {
    ScoredBlock sb1;
    sb1.rrf_score = 0.5f;

    ScoredBlock sb2;
    sb2.rrf_score = 0.3f;

    EXPECT_TRUE(sb1 > sb2);
    EXPECT_FALSE(sb2 > sb1);
}

TEST(ScoredBlockTest, ComparisonEqualScores) {
    ScoredBlock sb1;
    sb1.rrf_score = 0.5f;

    ScoredBlock sb2;
    sb2.rrf_score = 0.5f;

    EXPECT_FALSE(sb1 > sb2);
    EXPECT_FALSE(sb2 > sb1);
}

// Test basic field assignment
TEST(ScoredBlockTest, FieldAssignment) {
    ScoredBlock sb;
    sb.block_id = 123;
    sb.doc_id = "test-doc-456";
    sb.chunk_index = 7;
    sb.rrf_score = 0.8f;
    sb.hit_routes = kRouteVector | kRouteBM25;

    EXPECT_EQ(sb.block_id, 123);
    EXPECT_EQ(sb.doc_id, "test-doc-456");
    EXPECT_EQ(sb.chunk_index, 7);
    EXPECT_FLOAT_EQ(sb.rrf_score, 0.8f);
    EXPECT_EQ(sb.hit_count(), 2);
}

}  // namespace cortrix
