#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/retrieval/sparse_codec.h"
#include "cortrix/retrieval/sparse_fallback.h"
#include "cortrix/retrieval/sparse_metrics.h"
#include "cortrix/retrieval/sparse_rrf.h"
#include "cortrix/retrieval/splade_sparse_retriever.h"
#include "cortrix/spc/onnx_embedder.h"

// F40 S11 — standalone integration: the full F40 sparse pipeline wired together
// against the stub embedder + in-memory inverted index. Covers IT-F40-1/2/3/4/5/
// 7/8 (the ones that don't need a real model/BEIR dataset — those are D3.5):
//   embed (dense+sparse) -> serialize round-trip -> index Add -> Search ->
//   5-path RRF fuse -> L2 fallback degrade.
// Real ONNX inference + BEIR Recall@10 (+3pp) = D3.5 (see bench_f40_sparse +
// detailed-design §13.bis 2.3).
namespace cortrix::retrieval {
namespace {

// Embed a chunk with the (stub) BGE-M3 embedder and convert to a retrieval
// SparseVector, exercising the real S1 → S3 path.
SparseVector EmbedToSparse(cortrix::OnnxEmbedder& emb, const std::string& text,
                           int top_k = 100) {
    cortrix::EmbedWithSparseResult r;
    EXPECT_TRUE(emb.EmbedWithSparse(text, &r, top_k).ok());
    SparseVector v;
    v.terms = r.sparse;
    return v;
}

// IT-F40-1 — single inference dual output + sparse_vec round-trip.
TEST(F40SparseIntegrationTest, IT1_EmbedSerializeRoundTrip) {
    cortrix::OnnxEmbedder emb("", 128);
    ASSERT_TRUE(emb.Init().ok());

    cortrix::EmbedWithSparseResult r;
    ASSERT_TRUE(emb.EmbedWithSparse("Q3 2026 revenue grew 23 percent", &r).ok());
    EXPECT_EQ(static_cast<int>(r.dense.size()), 128);
    ASSERT_FALSE(r.sparse.empty());

    SparseVector v;
    v.terms = r.sparse;
    bool ok = false;
    auto blob = SerializeSparseVec(v, &ok);
    ASSERT_TRUE(ok);
    auto back = DeserializeSparseVec(blob);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value().terms, v.terms);  // round-trip exact
}

// IT-F40-2/4 — index N chunks then query returns scored candidates.
TEST(F40SparseIntegrationTest, IT2_IndexAndQuery) {
    cortrix::OnnxEmbedder emb("", 128);
    ASSERT_TRUE(emb.Init().ok());
    SpladeSparseRetriever idx(SpladeConfig{}, ":memory:");
    ASSERT_TRUE(idx.Open().ok());

    // Index 50 distinct chunks.
    std::vector<std::string> texts;
    for (int i = 0; i < 50; ++i) {
        std::string t = "alpha beta gamma chunk number " + std::to_string(i);
        texts.push_back(t);
        ASSERT_TRUE(idx.Add("ns", "c" + std::to_string(i), EmbedToSparse(emb, t)).ok());
    }
    // A query sharing tokens with the corpus returns hits.
    auto q = EmbedToSparse(emb, "alpha beta gamma chunk number 7");
    auto hits = idx.Search(q, "ns", 100);
    ASSERT_FALSE(hits.empty());
    // c7 (exact match) should score highest among them.
    EXPECT_EQ(hits.front().child_id, "c7");
}

// IT-F40-3 — delete removes from results.
TEST(F40SparseIntegrationTest, IT3_DeleteRemovesFromResults) {
    cortrix::OnnxEmbedder emb("", 64);
    ASSERT_TRUE(emb.Init().ok());
    SpladeSparseRetriever idx(SpladeConfig{}, ":memory:");
    ASSERT_TRUE(idx.Open().ok());

    auto v = EmbedToSparse(emb, "unique deletable content");
    ASSERT_TRUE(idx.Add("ns", "del", v).ok());
    ASSERT_FALSE(idx.Search(v, "ns", 10).empty());
    ASSERT_TRUE(idx.Remove("ns", "del").ok());
    EXPECT_TRUE(idx.Search(v, "ns", 10).empty());
}

// IT-F40-5 — 5-path RRF over a real sparse list + (mock) other paths.
TEST(F40SparseIntegrationTest, IT5_FivePathRrfWithRealSparse) {
    cortrix::OnnxEmbedder emb("", 64);
    ASSERT_TRUE(emb.Init().ok());
    SpladeSparseRetriever idx(SpladeConfig{}, ":memory:");
    ASSERT_TRUE(idx.Open().ok());

    for (int i = 0; i < 10; ++i) {
        idx.Add("ns", "c" + std::to_string(i),
                EmbedToSparse(emb, "doc body token set " + std::to_string(i)));
    }
    auto sparse_hits = idx.Search(EmbedToSparse(emb, "doc body token set 3"), "ns", 50);
    ASSERT_FALSE(sparse_hits.empty());

    FivePathInput in;
    in.sparse = sparse_hits;
    in.dense = {{"c3", 0.9f}, {"c1", 0.4f}};  // mock dense path
    in.fts5 = {{"c3", 0.8f}};                 // mock fts5 path
    // contextualized + hype empty (F35/F38 mock this round).
    auto fused = FuseFivePathRrf(in, /*top_n=*/10);
    ASSERT_FALSE(fused.empty());
    EXPECT_EQ(fused.front().child_id, "c3");  // strong across sparse+dense+fts5
}

// IT-F40-7 — L2 fallback: sparse unavailable → degrade to dense+fts5(+hype).
TEST(F40SparseIntegrationTest, IT7_L2FallbackDegrades) {
    // Retriever NOT opened → IsAvailable() false → fallback.
    SpladeSparseRetriever idx(SpladeConfig{}, ":memory:");
    EXPECT_FALSE(idx.IsAvailable());

    auto ex = DecideL2Fallback(idx.IsAvailable(), /*resolved_top_k=*/100);
    EXPECT_TRUE(ex.fallback_used);
    EXPECT_EQ(ex.via_path, "fallback_dense_fts5_hype");

    FivePathInput in;
    in.sparse = {{"sparse_only", 0.9f}};
    in.dense = {{"a", 0.5f}};
    in.fts5 = {{"a", 0.5f}};
    auto degraded = FuseFivePathRrf(DropSparsePath(in));
    bool sparse_only_present =
        std::any_of(degraded.begin(), degraded.end(),
                    [](const RrfFusedHit& h) { return h.child_id == "sparse_only"; });
    EXPECT_FALSE(sparse_only_present);  // dropped with the sparse path
}

// IT-F40-8 — posting-list cache hit-rate under repeated query load (≥70%).
TEST(F40SparseIntegrationTest, IT8_CacheHitRateUnderLoad) {
    cortrix::OnnxEmbedder emb("", 64);
    ASSERT_TRUE(emb.Init().ok());
    SpladeSparseRetriever idx(SpladeConfig{}, ":memory:");
    ASSERT_TRUE(idx.Open().ok());
    for (int i = 0; i < 20; ++i) {
        idx.Add("ns", "c" + std::to_string(i),
                EmbedToSparse(emb, "stable corpus tokens here " + std::to_string(i)));
    }
    auto q = EmbedToSparse(emb, "stable corpus tokens here");
    for (int i = 0; i < 20; ++i) (void)idx.Search(q, "ns", 50);
    EXPECT_GE(idx.cache_hit_rate(), 0.70);  // §verify ≥70%
}

// Metric integration: the pipeline can record the §10 metrics end-to-end.
TEST(F40SparseIntegrationTest, MetricsRecordedAlongPipeline) {
    SparseMetrics::Instance().ResetForTest();
    using IS = SparseMetrics::InferenceStatus;
    using VP = SparseMetrics::ViaPath;

    SparseMetrics::Instance().RecordInference(IS::kSuccess);
    SparseMetrics::Instance().RecordQueryViaPath(VP::kSparse);
    SparseMetrics::Instance().RecordQueryViaPath(VP::kFallbackDenseFts5);

    EXPECT_EQ(SparseMetrics::Instance().InferenceCount(IS::kSuccess), 1u);
    EXPECT_EQ(SparseMetrics::Instance().QueryViaPathCount(VP::kSparse), 1u);
    EXPECT_EQ(SparseMetrics::Instance().QueryViaPathCount(VP::kFallbackDenseFts5), 1u);
    SparseMetrics::Instance().ResetForTest();
}

}  // namespace
}  // namespace cortrix::retrieval
