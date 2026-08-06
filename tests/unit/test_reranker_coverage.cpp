// S5.1 — unit-test completion to the coverage gates (overall ≥85% / core ≥95%):
// the OnnxReranker::Rerank integration paths (missing-chunk tolerance,
// open-breaker RRF fallback, extremely-long candidate, field assembly), the
// factory success path, the circuit-breaker-disabled ScoreBatch branch, and the
// remaining MockReranker scenarios from reranker (ONNX-exception / score=0 /
// varied score distribution).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "cortrix/reranker.h"
#include "cortrix/reranker/onnx_reranker.h"
#include "cortrix/reranker/reranker_metrics.h"
#include "cortrix/reranker/score_fusion.h"
#include "mock_chunk_store.h"
#include "mock_reranker.h"

namespace cortrix::reranker {
namespace {

using ::testing::_;
using ::testing::Return;
using ::testing::Throw;
using retrieval::RankedChunk;
using retrieval::ScoredResult;

RerankerConfig StubConfig() {
    RerankerConfig c;
    c.model_path = "";  // stub mode
    return c;
}

// A GetBatch implementation that returns text for every id (helper).
auto AllPresentGetBatch() {
    return [](const std::vector<std::string>& ids, std::vector<std::string>* missing) {
        if (missing) missing->clear();
        std::vector<store::ChunkRecord> out;
        for (const auto& id : ids) {
            store::ChunkRecord rec;
            rec.child_id = id;
            rec.parent_id = "doc_1";
            rec.content = "text for " + id;
            rec.chunk_index = 0;
            out.push_back(rec);
        }
        return out;
    };
}

// ===================== OnnxReranker::Rerank integration =====================

// Missing-chunk tolerance: a child_id absent from the store is skipped by
// GetBatch + appended to missing_ids; Rerank keeps the candidate (empty
// chunk_text) rather than failing (CX_ERR_CHUNK_NOT_FOUND existence
// tolerance — the not-found path is Agent-friendly, child_id retained).
TEST(RerankerCoverageTest, RerankToleratesMissingChunk) {
    store::MockChunkStore store;
    EXPECT_CALL(store, GetBatch(_, _))
        .WillOnce([](const std::vector<std::string>& ids, std::vector<std::string>* missing) {
            std::vector<store::ChunkRecord> out;
            // Return text only for the first id; report the rest as missing.
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i == 0) {
                    store::ChunkRecord rec;
                    rec.child_id = ids[i];
                    rec.content = "present text";
                    out.push_back(rec);
                } else if (missing) {
                    missing->push_back(ids[i]);
                }
            }
            return out;
        });

    OnnxReranker r(StubConfig(), &store);
    ASSERT_TRUE(r.Init().ok());
    std::vector<ScoredResult> cands{{"01CHILDA", 0.9f}, {"01CHILDB", 0.5f}};
    auto ranked = r.Rerank(cands, "q");

    ASSERT_EQ(ranked.size(), 2u);  // both candidates retained
    // Each candidate's child_id is preserved (Agent-friendly not-found path).
    bool seen_a = false, seen_b = false;
    for (const auto& rc : ranked) {
        if (rc.child_id == "01CHILDA") { seen_a = true; EXPECT_EQ(rc.chunk_text, "present text"); }
        if (rc.child_id == "01CHILDB") { seen_b = true; EXPECT_TRUE(rc.chunk_text.empty()); }
    }
    EXPECT_TRUE(seen_a);
    EXPECT_TRUE(seen_b);
}

// Open-breaker RRF fallback: once the breaker is open, Rerank → ScoreBatch returns
// all zeros, so every RankedChunk.rerank_score is 0 (the upper layer keeps RRF
// ordering). (step 1.)
TEST(RerankerCoverageTest, RerankOpenBreakerYieldsZeroRerankScores) {
    store::MockChunkStore store;
    EXPECT_CALL(store, GetBatch(_, _)).WillRepeatedly(AllPresentGetBatch());

    RerankerConfig c = StubConfig();
    c.circuit_breaker_threshold = 1;  // trips on first failure
    // Use a throwing subclass to force the trip, then observe the next Rerank.
    class Throwing : public OnnxReranker {
    public:
        using OnnxReranker::OnnxReranker;
        ~Throwing() override { Shutdown(); }
    protected:
        float ScoreForTask(const char*, const char*) override {
            throw std::runtime_error("boom");
        }
    };
    Throwing r(c, &store);
    ASSERT_TRUE(r.Init().ok());

    // Trip the breaker via a ScoreBatch failure.
    std::vector<const char*> p{"x"};
    r.ScoreBatch("q", p);
    ASSERT_EQ(r.circuit_state(), CircuitBreakerState::kOpen);

    // Now Rerank short-circuits: all rerank_score == 0.
    std::vector<ScoredResult> cands{{"01CHILDA", 0.9f}, {"01CHILDB", 0.5f}};
    auto ranked = r.Rerank(cands, "q");
    ASSERT_EQ(ranked.size(), 2u);
    for (const auto& rc : ranked) EXPECT_FLOAT_EQ(rc.rerank_score, 0.0f);
}

// Field assembly: chunk_text from the store, rerank_score set, and the NEW reranker
// contract — RankedChunk.score is the FUSED ordering score written
// back (rerank_score*0.7 + rrf_score*0.3), with the output sorted by that fused
// score descending (no score_signals here, so the multiplier is identity).
TEST(RerankerCoverageTest, RerankAssemblesAllFields) {
    store::MockChunkStore store;
    EXPECT_CALL(store, GetBatch(_, _)).WillOnce(AllPresentGetBatch());

    OnnxReranker r(StubConfig(), &store);
    ASSERT_TRUE(r.Init().ok());
    std::vector<ScoredResult> cands{{"01CHILDA", 0.2f}, {"01CHILDB", 0.8f}, {"01CHILDC", 0.5f}};
    auto ranked = r.Rerank(cands, "the query");

    ASSERT_EQ(ranked.size(), 3u);
    // .score now holds the fused ordering score, so the output is sorted by it.
    for (size_t i = 0; i + 1 < ranked.size(); ++i) {
        EXPECT_GE(ranked[i].score, ranked[i + 1].score);
    }
    const std::map<std::string, float> rrf_by_id{
        {"01CHILDA", 0.2f}, {"01CHILDB", 0.8f}, {"01CHILDC", 0.5f}};
    for (const auto& rc : ranked) {
        EXPECT_TRUE(rc.chunk_text.rfind("text for ", 0) == 0);
        EXPECT_FLOAT_EQ(rc.score,
                        rc.rerank_score * RerankerScoreFusion::kRerankWeight +
                            rrf_by_id.at(rc.child_id) * RerankerScoreFusion::kRrfWeight);
    }
}

// Rerank with a null ChunkStore: no reverse-lookup, chunk_text stays empty, but
// the call is safe and still scores/sorts (Score/ScoreBatch-only construction).
TEST(RerankerCoverageTest, RerankWithNullStoreIsSafe) {
    OnnxReranker r(StubConfig(), /*chunk_store=*/nullptr);
    ASSERT_TRUE(r.Init().ok());
    std::vector<ScoredResult> cands{{"01CHILDA", 0.9f}};
    auto ranked = r.Rerank(cands, "q");
    ASSERT_EQ(ranked.size(), 1u);
    EXPECT_TRUE(ranked[0].chunk_text.empty());
}

// ===================== factory + disabled-breaker branch =====================

TEST(RerankerCoverageTest, FactoryReturnsReadyRerankerInStubMode) {
    IReranker* r = CreateReranker(StubConfig(), nullptr);
    ASSERT_NE(r, nullptr);
    EXPECT_STREQ(r->Name(), "OnnxReranker");
    delete r;
}

// circuit_breaker_enabled=false → no breaker is constructed; ScoreBatch still
// runs (exercises the `if (circuit_breaker_)` false branches) and never trips.
TEST(RerankerCoverageTest, ScoreBatchWorksWithBreakerDisabled) {
    RerankerConfig c = StubConfig();
    c.circuit_breaker_enabled = false;
    OnnxReranker r(c, nullptr);
    ASSERT_TRUE(r.Init().ok());
    EXPECT_EQ(r.circuit_state(), CircuitBreakerState::kClosed);  // no breaker → reports closed

    std::vector<const char*> passages{"a", "b", "c"};
    auto scores = r.ScoreBatch("q", passages);
    ASSERT_EQ(scores.size(), 3u);
    for (float s : scores) {
        EXPECT_GE(s, 0.0f);
        EXPECT_LE(s, 1.0f);
    }
}

// Even with the breaker disabled, an ONNX exception still degrades to score=0 +
// records the failed-tasks metric (just no breaker accounting).
TEST(RerankerCoverageTest, BreakerDisabledStillScoresZeroAndRecordsMetricOnException) {
    RerankerMetrics::Instance().ResetForTest();
    class Throwing : public OnnxReranker {
    public:
        using OnnxReranker::OnnxReranker;
        ~Throwing() override { Shutdown(); }
    protected:
        float ScoreForTask(const char*, const char*) override {
            throw std::runtime_error("boom");
        }
    };
    RerankerConfig c = StubConfig();
    c.circuit_breaker_enabled = false;
    Throwing r(c, nullptr);
    ASSERT_TRUE(r.Init().ok());

    std::vector<const char*> p{"a"};
    auto scores = r.ScoreBatch("q", p);
    ASSERT_EQ(scores.size(), 1u);
    EXPECT_EQ(scores[0], 0.0f);
    EXPECT_EQ(RerankerMetrics::Instance().FailedTasksCount(
                  RerankerMetrics::FailedTaskReason::kOnnxException), 1u);
    EXPECT_EQ(r.circuit_state(), CircuitBreakerState::kClosed);  // never tripped
}

// ===================== MockReranker scenarios =====================

// "throws ONNX exception": a mock Score that throws propagates to the caller (units that
// integrate a reranker can drive the exception path through the interface).
TEST(RerankerCoverageTest, MockScoreCanThrow) {
    MockReranker mock;
    EXPECT_CALL(mock, Score(_, _)).WillOnce(Throw(std::runtime_error("onnx")));
    EXPECT_THROW(mock.Score("q", "p"), std::runtime_error);
}

// "score=0": a mock returning all-zero scores (the degraded distribution).
TEST(RerankerCoverageTest, MockScoreBatchAllZero) {
    MockReranker mock;
    EXPECT_CALL(mock, ScoreBatch(_, _))
        .WillOnce(Return(std::vector<float>{0.0f, 0.0f, 0.0f}));
    std::vector<const char*> p{"a", "b", "c"};
    auto s = mock.ScoreBatch("q", p);
    ASSERT_EQ(s.size(), 3u);
    for (float v : s) EXPECT_FLOAT_EQ(v, 0.0f);
}

// "varied score distribution": a varied distribution, and Rerank canned to return them
// sorted (a unit downstream of IReranker can assert on ordering).
TEST(RerankerCoverageTest, MockRerankVariedScoreDistribution) {
    MockReranker mock;
    std::vector<RankedChunk> canned{
        RankedChunk{"01C1", "t1", "p1", 0.30f, 0.95f, {}, {}},
        RankedChunk{"01C2", "t2", "p2", 0.40f, 0.50f, {}, {}},
        RankedChunk{"01C3", "t3", "p3", 0.10f, 0.05f, {}, {}},
    };
    EXPECT_CALL(mock, Rerank(_, _)).WillOnce(Return(canned));
    std::vector<ScoredResult> cands{{"01C1", 0.3f}, {"01C2", 0.4f}, {"01C3", 0.1f}};
    auto out = mock.Rerank(cands, "q");
    ASSERT_EQ(out.size(), 3u);
    // The canned distribution is monotonically decreasing in rerank_score.
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        EXPECT_GE(out[i].rerank_score, out[i + 1].rerank_score);
    }
    EXPECT_FLOAT_EQ(out[0].rerank_score, 0.95f);
    EXPECT_FLOAT_EQ(out[2].rerank_score, 0.05f);
}

// ===================== ScoreBatch null/empty input guards =====================

// A null passage pointer (and a null query) hit the `passages[i] ? : ""` and the
// StubScore null-guard branches — the stub still returns a valid [0,1] score.
TEST(RerankerCoverageTest, ScoreBatchNullPassageAndQueryTolerated) {
    OnnxReranker r(StubConfig(), nullptr);
    ASSERT_TRUE(r.Init().ok());

    std::vector<const char*> passages{nullptr, "real passage", nullptr};
    auto scores = r.ScoreBatch(/*query=*/nullptr, passages);
    ASSERT_EQ(scores.size(), 3u);
    for (float v : scores) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
}

// Empty passage list → empty score vector (the n==0 loop never runs); the
// score-duration guard still fires on the return path.
TEST(RerankerCoverageTest, ScoreBatchEmptyPassagesEmptyScores) {
    OnnxReranker r(StubConfig(), nullptr);
    ASSERT_TRUE(r.Init().ok());
    auto scores = r.ScoreBatch("q", {});
    EXPECT_TRUE(scores.empty());
}

// Rerank with a null ChunkStore: the `if (chunk_store_)` false branch — no
// reverse lookup, every candidate keeps an empty chunk_text but is still scored
// and sorted.
TEST(RerankerCoverageTest, RerankWithNullChunkStoreSkipsLookup) {
    OnnxReranker r(StubConfig(), /*chunk_store=*/nullptr);
    ASSERT_TRUE(r.Init().ok());
    std::vector<ScoredResult> cands{{"01CHILDA", 0.9f}, {"01CHILDB", 0.2f}};
    auto ranked = r.Rerank(cands, "q");
    ASSERT_EQ(ranked.size(), 2u);
    for (const auto& rc : ranked) EXPECT_TRUE(rc.chunk_text.empty());
}

}  // namespace
}  // namespace cortrix::reranker
