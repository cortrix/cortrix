#include <gtest/gtest.h>
#include "cortrix/spc/onnx_embedder.h"
#include <cmath>

// Sparse retrieval S1 — OnnxEmbedder.EmbedWithSparse / EmbedBatchWithSparse.
// Standalone D3: dense reuses the frozen embed path; sparse is the deterministic
// stub (real BGE-M3 sparse-head extraction = D3.5). These tests pin the API
// contract + the stub's reproducibility / top-K behavior so the downstream
// serialize / inverted-index / RRF stories build on a stable foundation.
namespace cortrix {
namespace {

// ---------- null-pointer guards ----------

TEST(F40EmbedWithSparseTest, NullResultRejected) {
    OnnxEmbedder e("", 128);
    e.Init();
    Status s = e.EmbedWithSparse("hello", nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST(F40EmbedWithSparseTest, BatchNullResultsRejected) {
    OnnxEmbedder e("", 128);
    e.Init();
    Status s = e.EmbedBatchWithSparse({"hello"}, nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST(F40EmbedWithSparseTest, NotInitializedFails) {
    OnnxEmbedder e("", 128);  // no Init()
    EmbedWithSparseResult r;
    Status s = e.EmbedWithSparse("hello", &r);
    EXPECT_FALSE(s.ok());  // dense half (Embed) fails without Init
}

// ---------- dense + sparse both produced ----------

TEST(F40EmbedWithSparseTest, ProducesDenseAndSparse) {
    OnnxEmbedder e("", 128);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult r;
    Status s = e.EmbedWithSparse("Q3 2026 revenue grew sharply", &r);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(static_cast<int>(r.dense.size()), 128);
    EXPECT_FALSE(r.sparse.empty());
    EXPECT_GT(r.inference_time_ms, 0.0f);
}

TEST(F40EmbedWithSparseTest, DenseIsUnitNormalized) {
    OnnxEmbedder e("", 256);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult r;
    ASSERT_TRUE(e.EmbedWithSparse("normalize me", &r).ok());

    float norm = 0.0f;
    for (float v : r.dense) norm += v * v;
    EXPECT_NEAR(std::sqrt(norm), 1.0f, 0.001f);
}

TEST(F40EmbedWithSparseTest, SparseWeightsStrictlyPositive) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult r;
    ASSERT_TRUE(e.EmbedWithSparse("alpha beta gamma delta", &r).ok());
    ASSERT_FALSE(r.sparse.empty());
    for (const auto& [term_id, weight] : r.sparse) {
        EXPECT_GT(weight, 0.0f);
        EXPECT_GT(term_id, 0u);          // id 0 reserved as "no term"
        EXPECT_LE(term_id, 65535u);      // uint16 serialization window
    }
}

// ---------- determinism (stub mirrors StubEmbed) ----------

TEST(F40EmbedWithSparseTest, DeterministicForSameInput) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult a, b;
    ASSERT_TRUE(e.EmbedWithSparse("deterministic sparse test", &a).ok());
    ASSERT_TRUE(e.EmbedWithSparse("deterministic sparse test", &b).ok());

    EXPECT_EQ(a.dense, b.dense);
    EXPECT_EQ(a.sparse, b.sparse);
}

TEST(F40EmbedWithSparseTest, DifferentInputsDifferentSparse) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult a, b;
    ASSERT_TRUE(e.EmbedWithSparse("the quick brown fox", &a).ok());
    ASSERT_TRUE(e.EmbedWithSparse("a totally separate phrase", &b).ok());
    EXPECT_NE(a.sparse, b.sparse);
}

// ---------- empty / whitespace content ----------

TEST(F40EmbedWithSparseTest, EmptyTextEmptySparse) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult r;
    ASSERT_TRUE(e.EmbedWithSparse("", &r).ok());
    EXPECT_TRUE(r.sparse.empty());          // dead chunk → caller writes NULL
    EXPECT_EQ(static_cast<int>(r.dense.size()), 64);  // dense still produced
}

TEST(F40EmbedWithSparseTest, WhitespaceOnlyEmptySparse) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult r;
    ASSERT_TRUE(e.EmbedWithSparse("   \t  \n ", &r).ok());
    EXPECT_TRUE(r.sparse.empty());
}

// ---------- top-K truncation ------------------

TEST(F40EmbedWithSparseTest, TopKTruncates) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    // 20 distinct tokens → at most 20 sparse terms; cap to 5.
    std::string many = "t01 t02 t03 t04 t05 t06 t07 t08 t09 t10 "
                       "t11 t12 t13 t14 t15 t16 t17 t18 t19 t20";
    EmbedWithSparseResult r;
    ASSERT_TRUE(e.EmbedWithSparse(many, &r, /*top_k=*/5).ok());
    EXPECT_EQ(r.sparse.size(), 5u);
}

TEST(F40EmbedWithSparseTest, TopKKeepsHighestWeights) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    std::string many = "aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo";
    EmbedWithSparseResult full, capped;
    ASSERT_TRUE(e.EmbedWithSparse(many, &full, /*top_k=*/0).ok());   // keep all
    ASSERT_TRUE(e.EmbedWithSparse(many, &capped, /*top_k=*/3).ok());
    ASSERT_GT(full.sparse.size(), 3u);
    EXPECT_EQ(capped.sparse.size(), 3u);

    // The 3 kept weights must be >= every dropped weight.
    float min_kept = 1e30f;
    for (const auto& [id, w] : capped.sparse) min_kept = std::min(min_kept, w);
    for (const auto& [id, w] : full.sparse) {
        if (capped.sparse.count(id) == 0) {
            EXPECT_LE(w, min_kept + 1e-6f);
        }
    }
}

TEST(F40EmbedWithSparseTest, TopKZeroKeepsAll) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult r;
    ASSERT_TRUE(e.EmbedWithSparse("one two three", &r, /*top_k=*/0).ok());
    EXPECT_EQ(r.sparse.size(), 3u);  // 3 distinct tokens, none dropped
}

TEST(F40EmbedWithSparseTest, DuplicateTokensAccumulateWeight) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult once, twice;
    ASSERT_TRUE(e.EmbedWithSparse("repeat", &once, /*top_k=*/0).ok());
    ASSERT_TRUE(e.EmbedWithSparse("repeat repeat", &twice, /*top_k=*/0).ok());
    ASSERT_EQ(once.sparse.size(), 1u);
    ASSERT_EQ(twice.sparse.size(), 1u);
    auto id = once.sparse.begin()->first;
    EXPECT_GT(twice.sparse.at(id), once.sparse.at(id));  // tf accumulation
}

// ---------- batch form ----------

TEST(F40EmbedWithSparseTest, BatchProducesPerItem) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    std::vector<std::string> texts = {"first chunk", "second chunk", "third"};
    std::vector<EmbedWithSparseResult> out;
    ASSERT_TRUE(e.EmbedBatchWithSparse(texts, &out).ok());
    ASSERT_EQ(out.size(), 3u);
    for (const auto& r : out) {
        EXPECT_EQ(static_cast<int>(r.dense.size()), 64);
        EXPECT_FALSE(r.sparse.empty());
    }
}

TEST(F40EmbedWithSparseTest, BatchEmptyList) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    std::vector<EmbedWithSparseResult> out;
    ASSERT_TRUE(e.EmbedBatchWithSparse({}, &out).ok());
    EXPECT_TRUE(out.empty());
}

TEST(F40EmbedWithSparseTest, BatchMatchesSingle) {
    OnnxEmbedder e("", 64);
    ASSERT_TRUE(e.Init().ok());

    EmbedWithSparseResult single;
    ASSERT_TRUE(e.EmbedWithSparse("consistency check", &single).ok());

    std::vector<EmbedWithSparseResult> out;
    ASSERT_TRUE(e.EmbedBatchWithSparse({"consistency check"}, &out).ok());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].dense, single.dense);
    EXPECT_EQ(out[0].sparse, single.sparse);
}

}  // namespace
}  // namespace cortrix
