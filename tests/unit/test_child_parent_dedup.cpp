#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/chunker/child_parent_dedup.h"

namespace cortrix::chunker {
namespace {

ChildHit Hit(const std::string& cid, const std::string& pid, float score) {
    return ChildHit{cid, pid, score};
}

TEST(ChildParentDedupTest, EmptyInput) {
    auto g = DedupChildrenToParents({}, 3);
    EXPECT_TRUE(g.empty());
    EXPECT_TRUE(BuildChildrenHitsMeta(g).empty());
}

TEST(ChildParentDedupTest, SingleChildSingleParent) {
    auto g = DedupChildrenToParents({Hit("C1", "P1", 0.9f)}, 3);
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0].parent_id, "P1");
    EXPECT_EQ(g[0].primary_child_id, "C1");
    EXPECT_EQ(g[0].rerank_child_ids, (std::vector<std::string>{"C1"}));
    EXPECT_FLOAT_EQ(g[0].top_score, 0.9f);
}

TEST(ChildParentDedupTest, CTop3KeepsAtMostThreePerParent) {
    // 5 children of P1; C top-3 keeps the 3 highest-scoring for rerank, records all 5.
    std::vector<ChildHit> hits = {
        Hit("C1", "P1", 0.5f), Hit("C2", "P1", 0.9f), Hit("C3", "P1", 0.7f),
        Hit("C4", "P1", 0.6f), Hit("C5", "P1", 0.8f)};
    auto g = DedupChildrenToParents(hits, 3);
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0].primary_child_id, "C2");  // 0.9 highest
    // rerank set = top-3 by score: C2(0.9), C5(0.8), C3(0.7)
    EXPECT_EQ(g[0].rerank_child_ids, (std::vector<std::string>{"C2", "C5", "C3"}));
    // all hits recorded, score desc
    EXPECT_EQ(g[0].all_hit_child_ids, (std::vector<std::string>{"C2", "C5", "C3", "C4", "C1"}));
}

TEST(ChildParentDedupTest, GucControlsCap) {
    std::vector<ChildHit> hits = {
        Hit("C1", "P1", 0.5f), Hit("C2", "P1", 0.9f), Hit("C3", "P1", 0.7f)};
    auto g1 = DedupChildrenToParents(hits, 1);
    ASSERT_EQ(g1.size(), 1u);
    EXPECT_EQ(g1[0].rerank_child_ids, (std::vector<std::string>{"C2"}));  // cap=1

    auto g5 = DedupChildrenToParents(hits, 5);  // cap > group size
    EXPECT_EQ(g5[0].rerank_child_ids.size(), 3u);  // all 3
}

TEST(ChildParentDedupTest, CapClampedToAtLeastOne) {
    // A 0/negative cap must never drop all children (GUC validation enforces [1,10],
    // but the function defends regardless).
    auto g = DedupChildrenToParents({Hit("C1", "P1", 0.5f)}, 0);
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0].rerank_child_ids.size(), 1u);
}

TEST(ChildParentDedupTest, MultipleParentsOrderedByTopScore) {
    std::vector<ChildHit> hits = {
        Hit("A1", "PA", 0.4f),
        Hit("B1", "PB", 0.95f),
        Hit("C1", "PC", 0.6f)};
    auto g = DedupChildrenToParents(hits, 3);
    ASSERT_EQ(g.size(), 3u);
    EXPECT_EQ(g[0].parent_id, "PB");  // 0.95
    EXPECT_EQ(g[1].parent_id, "PC");  // 0.6
    EXPECT_EQ(g[2].parent_id, "PA");  // 0.4
}

TEST(ChildParentDedupTest, ScoreTieBrokenByChildIdDeterministically) {
    std::vector<ChildHit> hits = {
        Hit("Cb", "P1", 0.5f), Hit("Ca", "P1", 0.5f)};
    auto g = DedupChildrenToParents(hits, 3);
    ASSERT_EQ(g.size(), 1u);
    // equal score → child_id asc, so Ca is primary
    EXPECT_EQ(g[0].primary_child_id, "Ca");
    EXPECT_EQ(g[0].all_hit_child_ids, (std::vector<std::string>{"Ca", "Cb"}));
}

TEST(ChildParentDedupTest, FlatHitsEmptyParentEachOwnGroup) {
    // Flat-fallback children carry empty parent_id; each is its own group.
    std::vector<ChildHit> hits = {Hit("F1", "", 0.8f), Hit("F2", "", 0.9f)};
    auto g = DedupChildrenToParents(hits, 3);
    ASSERT_EQ(g.size(), 2u);
    EXPECT_TRUE(g[0].parent_id.empty());
    EXPECT_EQ(g[0].primary_child_id, "F2");  // higher score first
    EXPECT_EQ(g[1].primary_child_id, "F1");
}

TEST(ChildParentDedupTest, EqualTopScoreGroupsOrderedByParentId) {
    // Two parents whose top child has the SAME score → group order tie-break by
    // parent_id asc (deterministic). Exercises the group comparator tie path.
    std::vector<ChildHit> hits = {
        Hit("X1", "PZ", 0.5f), Hit("Y1", "PA", 0.5f)};
    auto g = DedupChildrenToParents(hits, 3);
    ASSERT_EQ(g.size(), 2u);
    EXPECT_EQ(g[0].parent_id, "PA");  // equal score → parent_id asc
    EXPECT_EQ(g[1].parent_id, "PZ");
}

TEST(ChildParentDedupTest, EqualTopScoreFlatGroupsOrderedByChildId) {
    // Equal-score flat hits (empty parent_id) tie-break by primary_child id.
    std::vector<ChildHit> hits = {Hit("Fz", "", 0.5f), Hit("Fa", "", 0.5f)};
    auto g = DedupChildrenToParents(hits, 3);
    ASSERT_EQ(g.size(), 2u);
    EXPECT_EQ(g[0].primary_child_id, "Fa");
    EXPECT_EQ(g[1].primary_child_id, "Fz");
}

TEST(ChildParentDedupTest, ChildrenHitsMetaReflectsAllHits) {
    std::vector<ChildHit> hits = {
        Hit("C3", "P1", 0.7f), Hit("C7", "P1", 0.9f), Hit("C12", "P1", 0.8f)};
    auto g = DedupChildrenToParents(hits, 3);
    auto meta = BuildChildrenHitsMeta(g);
    ASSERT_EQ(meta.size(), 1u);
    EXPECT_EQ(meta[0].parent_id, "P1");
    EXPECT_EQ(meta[0].primary_child, "C7");
    // matches the § 3.3 example shape {parent_id:"P01", hits:["C03","C07","C12"], primary_child:"C07"}
    EXPECT_EQ(meta[0].hits, (std::vector<std::string>{"C7", "C12", "C3"}));
}

}  // namespace
}  // namespace cortrix::chunker
