// S1.2 — OnnxReranker skeleton: shared Ort::Env singleton + shared HfTokenizer
// registry + independent Session wiring (stub mode offline, no model file).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "cortrix/ml/ort_env_singleton.h"
#include "cortrix/ml/tokenizer_registry.h"
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
    c.model_path = "";  // no model → stub mode (offline standalone)
    return c;
}

// --- OrtEnvSingleton (§2.4-bis: std::call_once, one Env per process) ---

TEST(OrtEnvSingletonTest, InitIsIdempotentAndMarksInitialized) {
    ml::OrtEnvSingleton::Init();
    EXPECT_TRUE(ml::OrtEnvSingleton::Initialized());
    // Second call returns the same handle (call_once → never re-created).
    void* h1 = ml::OrtEnvSingleton::EnvHandle();
    void* h2 = ml::OrtEnvSingleton::EnvHandle();
    EXPECT_EQ(h1, h2);
}

// --- TokenizerRegistry (§2.4-bis: shared, mutex on register, lock-free read) ---

TEST(TokenizerRegistryTest, MissingKeyReturnsNull) {
    ml::TokenizerRegistry::ResetForTest();
    EXPECT_EQ(ml::TokenizerRegistry::Get("no-such-key"), nullptr);
}

TEST(TokenizerRegistryTest, LoadFailureSurfacesAndRegistersNothing) {
    ml::TokenizerRegistry::ResetForTest();
    Status s = ml::TokenizerRegistry::LoadAndRegister("bge-m3", "/nonexistent/tokenizer.json");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(ml::TokenizerRegistry::Get("bge-m3"), nullptr);
}

// --- OnnxReranker skeleton ---

TEST(OnnxRerankerSkeletonTest, ConstructsInStubModeWithoutModel) {
    OnnxReranker r(StubConfig(), /*chunk_store=*/nullptr);
    Status s = r.Init();
    EXPECT_TRUE(s.ok());
    EXPECT_FALSE(r.is_real_model());     // stub mode, no Session
    EXPECT_STREQ(r.Name(), "OnnxReranker");
}

TEST(OnnxRerankerSkeletonTest, ScoreBatchMatchesPassageCountAndIsDeterministic) {
    OnnxReranker r(StubConfig(), nullptr);
    ASSERT_TRUE(r.Init().ok());
    std::vector<const char*> passages{"alpha", "beta", "gamma"};
    auto s1 = r.ScoreBatch("query", passages);
    auto s2 = r.ScoreBatch("query", passages);
    ASSERT_EQ(s1.size(), 3u);
    EXPECT_EQ(s1, s2);  // deterministic stub
    for (float v : s1) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
}

TEST(OnnxRerankerSkeletonTest, RerankReversesLookupViaChunkStoreAndSortsByFusedScore) {
    store::MockChunkStore store;
    // GetBatch returns chunk text for both child ids, in input order.
    EXPECT_CALL(store, GetBatch(_, _))
        .WillOnce([](const std::vector<std::string>& ids, std::vector<std::string>* missing) {
            (void)missing;
            std::vector<store::ChunkRecord> out;
            for (const auto& id : ids) {
                store::ChunkRecord rec;
                rec.child_id = id;
                rec.parent_id = "doc_1";
                rec.content = "text for " + id;
                out.push_back(rec);
            }
            return out;
        });

    OnnxReranker r(StubConfig(), &store);
    ASSERT_TRUE(r.Init().ok());

    std::vector<ScoredResult> cands{{"01CHILDA", 0.9f}, {"01CHILDB", 0.1f}};
    auto ranked = r.Rerank(cands, "the query");

    ASSERT_EQ(ranked.size(), 2u);
    // Both chunks got their text reverse-looked-up.
    for (const auto& rc : ranked) {
        EXPECT_FALSE(rc.chunk_text.empty());
        EXPECT_TRUE(rc.chunk_text.rfind("text for ", 0) == 0);
    }
    // Cross-encoder order preserved on rerank_score.
    EXPECT_GE(ranked[0].rerank_score, ranked[1].rerank_score);
    // New reranker contract (§4.2-ter): RankedChunk.score is the FUSED ordering score
    // written back (rerank*0.7 + rrf*0.3; no score_signals here), so the output is
    // sorted by the fused score and `score` is no longer the raw pre-rerank RRF.
    EXPECT_GE(ranked[0].score, ranked[1].score);
    const std::map<std::string, float> rrf_by_id{{"01CHILDA", 0.9f}, {"01CHILDB", 0.1f}};
    for (const auto& rc : ranked) {
        EXPECT_FLOAT_EQ(rc.score,
                        rc.rerank_score * RerankerScoreFusion::kRerankWeight +
                            rrf_by_id.at(rc.child_id) * RerankerScoreFusion::kRrfWeight);
    }
}

TEST(OnnxRerankerSkeletonTest, RerankEmptyCandidatesReturnsEmpty) {
    OnnxReranker r(StubConfig(), nullptr);
    ASSERT_TRUE(r.Init().ok());
    auto ranked = r.Rerank({}, "q");
    EXPECT_TRUE(ranked.empty());
}

}  // namespace
}  // namespace cortrix::reranker
