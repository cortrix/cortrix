// F02 §4.2-ter — RerankerScoreFusion (F02-owned RRF fusion) + Rerank ordering.
//
// Covers (Agent-D F02 wrap-up increment Task A):
//   (1) Fusion math: weighted_sum(rerank*0.7 + rrf*0.3) + named-constant weights.
//   (2) Rerank sorts by the FUSED score (not raw rerank_score): a candidate with
//       high rerank / low RRF can be re-ordered past / behind one with low rerank /
//       high RRF, and the RRF component can flip the rerank-only order.
//
// The fused score is local sort-use only — RankedChunk fields are unchanged.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "cortrix/reranker/onnx_reranker.h"
#include "cortrix/reranker/score_fusion.h"
#include "mock_chunk_store.h"

namespace cortrix::reranker {
namespace {

using ::testing::_;
using retrieval::RankedChunk;
using retrieval::ScoredResult;

RerankerConfig StubConfig() {
    RerankerConfig c;
    c.model_path = "";  // stub mode
    return c;
}

// GetBatch returning "text for <id>" for every candidate, in input order.
auto AllPresentGetBatch() {
    return [](const std::vector<std::string>& ids, std::vector<std::string>* missing) {
        if (missing) missing->clear();
        std::vector<store::ChunkRecord> out;
        for (const auto& id : ids) {
            store::ChunkRecord rec;
            rec.child_id = id;
            rec.parent_id = "doc_1";
            rec.content = "text for " + id;
            out.push_back(rec);
        }
        return out;
    };
}

// OnnxReranker whose per-passage cross-encoder score is injected by child id so a
// crafted (rerank, rrf) matrix can be exercised. Passage text is "text for <id>".
class ScriptedReranker : public OnnxReranker {
public:
    ScriptedReranker(const RerankerConfig& c, store::ChunkStore* s,
                     std::map<std::string, float> scores)
        : OnnxReranker(c, s), scores_(std::move(scores)) {}

protected:
    float ScoreForTask(const char* /*query*/, const char* passage) override {
        const std::string p = passage ? passage : "";
        const std::string prefix = "text for ";
        const std::string id = (p.rfind(prefix, 0) == 0) ? p.substr(prefix.size()) : p;
        auto it = scores_.find(id);
        return it != scores_.end() ? it->second : 0.0f;
    }

private:
    std::map<std::string, float> scores_;
};

// ---- (1) Fusion math -------------------------------------------------------

TEST(RerankerScoreFusionTest, WeightsAreV1Contract) {
    EXPECT_FLOAT_EQ(RerankerScoreFusion::kRerankWeight, 0.7f);
    EXPECT_FLOAT_EQ(RerankerScoreFusion::kRrfWeight, 0.3f);
}

TEST(RerankerScoreFusionTest, ComputeRerankRrfScoreIsWeightedSum) {
    RerankerScoreFusion fusion;
    RankedChunk chunk;  // unused by V1 fusion
    // 0.8*0.7 + 0.4*0.3 = 0.56 + 0.12 = 0.68
    EXPECT_FLOAT_EQ(fusion.ComputeRerankRrfScore(0.8f, 0.4f, chunk, "q"), 0.68f);
    // boundary: both 1 → 1; both 0 → 0.
    EXPECT_FLOAT_EQ(fusion.ComputeRerankRrfScore(1.0f, 1.0f, chunk, "q"), 1.0f);
    EXPECT_FLOAT_EQ(fusion.ComputeRerankRrfScore(0.0f, 0.0f, chunk, "q"), 0.0f);
    // pure-rerank (rrf=0) and pure-rrf (rerank=0) reduce to the single weight.
    EXPECT_FLOAT_EQ(fusion.ComputeRerankRrfScore(1.0f, 0.0f, chunk, "q"), 0.7f);
    EXPECT_FLOAT_EQ(fusion.ComputeRerankRrfScore(0.0f, 1.0f, chunk, "q"), 0.3f);
}

// ---- (2) Rerank sorts by the FUSED score ----------------------------------

// HIGH rerank + LOW rrf  vs  LOW rerank + HIGH rrf:
//   A: rerank 0.90, rrf 0.00 → fused 0.630
//   B: rerank 0.10, rrf 1.00 → fused 0.370
// Fused order A > B (A wins on the 0.7-weighted rerank), even though B dominates
// on raw RRF. Input order is B,A to prove sorting actually happens.
TEST(RerankerScoreFusionTest, RerankRanksByFusedScoreNotRawRrf) {
    store::MockChunkStore store;
    EXPECT_CALL(store, GetBatch(_, _)).WillOnce(AllPresentGetBatch());

    ScriptedReranker r(StubConfig(), &store,
                       {{"01CHILDA", 0.90f}, {"01CHILDB", 0.10f}});
    ASSERT_TRUE(r.Init().ok());

    std::vector<ScoredResult> cands{{"01CHILDB", 1.0f}, {"01CHILDA", 0.0f}};
    auto ranked = r.Rerank(cands, "q");

    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].child_id, "01CHILDA");  // fused 0.63 > 0.37
    EXPECT_EQ(ranked[1].child_id, "01CHILDB");
    // RankedChunk fields unchanged: raw rerank_score + pre-rerank RRF score kept.
    EXPECT_FLOAT_EQ(ranked[0].rerank_score, 0.90f);
    EXPECT_FLOAT_EQ(ranked[0].score, 0.0f);
    EXPECT_FLOAT_EQ(ranked[1].rerank_score, 0.10f);
    EXPECT_FLOAT_EQ(ranked[1].score, 1.0f);
}

// RRF component FLIPS a rerank-only order:
//   P: rerank 0.50, rrf 0.00 → fused 0.350
//   Q: rerank 0.40, rrf 1.00 → fused 0.580
// By raw rerank_score P (0.50) > Q (0.40); by fused score Q (0.58) > P (0.35).
// Asserts the output is NOT in rerank_score order — i.e. fusion is applied.
TEST(RerankerScoreFusionTest, RrfComponentCanFlipRerankOnlyOrder) {
    store::MockChunkStore store;
    EXPECT_CALL(store, GetBatch(_, _)).WillOnce(AllPresentGetBatch());

    ScriptedReranker r(StubConfig(), &store,
                       {{"01CHILDP", 0.50f}, {"01CHILDQ", 0.40f}});
    ASSERT_TRUE(r.Init().ok());

    std::vector<ScoredResult> cands{{"01CHILDP", 0.0f}, {"01CHILDQ", 1.0f}};
    auto ranked = r.Rerank(cands, "q");

    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].child_id, "01CHILDQ");  // fused 0.58 wins
    EXPECT_EQ(ranked[1].child_id, "01CHILDP");
    // The winner has the LOWER raw rerank_score → proves sort is by fused score.
    EXPECT_LT(ranked[0].rerank_score, ranked[1].rerank_score);
}

// Stable on ties: equal fused scores preserve input order.
TEST(RerankerScoreFusionTest, EqualFusedScoresPreserveInputOrder) {
    store::MockChunkStore store;
    EXPECT_CALL(store, GetBatch(_, _)).WillOnce(AllPresentGetBatch());

    // Both fuse to 0.5*0.7 + 0.5*0.3 = 0.5.
    ScriptedReranker r(StubConfig(), &store,
                       {{"01CHILD1", 0.5f}, {"01CHILD2", 0.5f}});
    ASSERT_TRUE(r.Init().ok());

    std::vector<ScoredResult> cands{{"01CHILD1", 0.5f}, {"01CHILD2", 0.5f}};
    auto ranked = r.Rerank(cands, "q");

    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].child_id, "01CHILD1");  // tie → input order kept
    EXPECT_EQ(ranked[1].child_id, "01CHILD2");
}

}  // namespace
}  // namespace cortrix::reranker
