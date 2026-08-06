#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "cortrix/common/block_types.h"
#include "cortrix/spc/hype_fusion.h"

// HyPE S4 — chunk-level RRF + by-parent dedup over a mixed P-HNSW candidate pool
// + the 3 B-class explain fields. Real P-HNSW split / QueryPipeline
// = integration.
namespace cortrix::spc {
namespace {

// A "chunk" candidate = any non-hype block_type. The real CortrixBlockType has
// no kBlockChunk=0 (content blocks use kBlockFile=1 etc.); the fusion split is
// purely "== kBlockHypeQuestion(16) ? hype : chunk", so 0 is a fine chunk-path
// sentinel here (matches's chunk-path treatment).
HypeCandidate Chunk(std::string child, float score) {
    return {child, /*block_type=*/0, score, "", ""};
}
HypeCandidate Hype(std::string source_child, float score, std::string q) {
    HypeCandidate c;
    c.child_id = "hypeblk";  // the hype Block's own id (unused for scoring)
    c.block_type = cortrix::kBlockHypeQuestion;
    c.score = score;
    c.source_child_id = std::move(source_child);
    c.question_text = std::move(q);
    return c;
}

const HypeFusedResult* Find(const std::vector<HypeFusedResult>& v,
                            const std::string& id) {
    for (const auto& r : v) if (r.child_id == id) return &r;
    return nullptr;
}

// ---------- chunk-only ----------

TEST(HypeFusionTest, ChunkOnlyRrf) {
    std::vector<HypeCandidate> pool = {Chunk("a", 0.9f), Chunk("b", 0.5f)};
    auto out = FuseHypeRecall(pool, {});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].child_id, "a");                 // rank0 > rank1
    EXPECT_FALSE(out[0].via_hype);                   // chunk path only
    EXPECT_FLOAT_EQ(out[0].rrf_score, 1.0f / 60.0f);
}

// ---------- hype-only maps to source_child_id ----------

TEST(HypeFusionTest, HypeMapsToSourceChild) {
    std::vector<HypeCandidate> pool = {Hype("chunk_x", 0.95f, "What is X?")};
    auto out = FuseHypeRecall(pool, {});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].child_id, "chunk_x");           // mapped to the source chunk
    EXPECT_TRUE(out[0].via_hype);
    EXPECT_EQ(out[0].hype_question_matched, "What is X?");
    EXPECT_FLOAT_EQ(out[0].hype_match_score, 0.95f);
}

// ---------- mixed: same child via chunk + hype accumulates ----------

TEST(HypeFusionTest, ChunkAndHypeSameChildAccumulates) {
    // "c1" recalled both as a chunk (rank0) and via a hype question (rank0).
    std::vector<HypeCandidate> pool = {
        Chunk("c1", 0.8f),
        Hype("c1", 0.92f, "How much revenue?"),
    };
    auto out = FuseHypeRecall(pool, {});
    ASSERT_EQ(out.size(), 1u);
    const HypeFusedResult* c1 = Find(out, "c1");
    ASSERT_TRUE(c1);
    EXPECT_TRUE(c1->via_hype);                        // got a hype contribution
    EXPECT_EQ(c1->hype_question_matched, "How much revenue?");
    // chunk rank0 + hype rank0 = 1/60 + 1/60.
    EXPECT_FLOAT_EQ(c1->rrf_score, 1.0f / 60 + 1.0f / 60);
}

TEST(HypeFusionTest, HypeBoostsRankAboveChunkOnly) {
    // c_both has chunk + hype; c_chunk has only chunk at a better chunk-rank.
    std::vector<HypeCandidate> pool = {
        Chunk("c_chunk", 0.9f),   // chunk rank0
        Chunk("c_both", 0.5f),    // chunk rank1
        Hype("c_both", 0.99f, "q"),  // hype rank0
    };
    auto out = FuseHypeRecall(pool, {});
    // c_both: 1/61 (chunk r1) + 1/60 (hype r0); c_chunk: 1/60 (chunk r0).
    const HypeFusedResult* both = Find(out, "c_both");
    const HypeFusedResult* chunk = Find(out, "c_chunk");
    ASSERT_TRUE(both && chunk);
    EXPECT_GT(both->rrf_score, chunk->rrf_score);     // hype lifts c_both to #1
    EXPECT_EQ(out[0].child_id, "c_both");
}

// ---------- best hype question selection ----------

TEST(HypeFusionTest, BestHypeQuestionWins) {
    // c1 matched by two hype questions; the higher-score one is the explain.
    std::vector<HypeCandidate> pool = {
        Hype("c1", 0.70f, "weaker question"),
        Hype("c1", 0.96f, "stronger question"),
    };
    auto out = FuseHypeRecall(pool, {});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].hype_question_matched, "stronger question");
    EXPECT_FLOAT_EQ(out[0].hype_match_score, 0.96f);
}

// ---------- by-parent dedup ----------

TEST(HypeFusionTest, ByParentDedupKeepsHighest) {
    // c1 and c2 share parent p1; only the higher rrf survives dedup.
    std::vector<HypeCandidate> pool = {Chunk("c1", 0.9f), Chunk("c2", 0.5f)};
    std::map<ChildId, ParentId> parent_of = {{"c1", "p1"}, {"c2", "p1"}};
    auto out = FuseHypeRecall(pool, parent_of);
    ASSERT_EQ(out.size(), 1u);                        // deduped to one per parent
    EXPECT_EQ(out[0].child_id, "c1");                 // c1 (rank0) beat c2 (rank1)
    EXPECT_EQ(out[0].parent_id, "p1");
}

TEST(HypeFusionTest, DifferentParentsBothKept) {
    std::vector<HypeCandidate> pool = {Chunk("c1", 0.9f), Chunk("c2", 0.5f)};
    std::map<ChildId, ParentId> parent_of = {{"c1", "p1"}, {"c2", "p2"}};
    auto out = FuseHypeRecall(pool, parent_of);
    EXPECT_EQ(out.size(), 2u);                        // distinct parents → both kept
}

TEST(HypeFusionTest, UnknownParentIsOwnGroup) {
    std::vector<HypeCandidate> pool = {Chunk("c1", 0.9f), Chunk("c2", 0.5f)};
    auto out = FuseHypeRecall(pool, {});              // no parent map
    EXPECT_EQ(out.size(), 2u);                        // each its own group
}

// ---------- top-N + edge ----------

TEST(HypeFusionTest, TopNTruncates) {
    std::vector<HypeCandidate> pool = {Chunk("a", 0.9f), Chunk("b", 0.5f),
                                       Chunk("c", 0.3f)};
    auto out = FuseHypeRecall(pool, {}, /*top_n=*/2);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].child_id, "a");
    EXPECT_EQ(out[1].child_id, "b");
}

TEST(HypeFusionTest, EmptyPoolEmptyOut) {
    EXPECT_TRUE(FuseHypeRecall({}, {}).empty());
}

TEST(HypeFusionTest, RrfKZeroGuarded) {
    std::vector<HypeCandidate> pool = {Chunk("a", 0.9f)};
    auto out = FuseHypeRecall(pool, {}, 0, /*rrf_k=*/0);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].rrf_score, 1.0f / 60.0f);  // guarded to 60, not div0
}

}  // namespace
}  // namespace cortrix::spc
