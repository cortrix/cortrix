#include <gtest/gtest.h>
#include "cortrix/spc/onnx_embedder.h"
#include <cmath>

namespace cortrix {
namespace {

// ============================================================
// Construction and Init tests
// ============================================================

TEST(OnnxEmbedderTest, ConstructorDefaults) {
    OnnxEmbedder embedder("model.onnx");
    EXPECT_EQ(embedder.dimension(), 1024);
}

TEST(OnnxEmbedderTest, ConstructorCustomDim) {
    OnnxEmbedder embedder("model.onnx", 384, 2, 1);
    EXPECT_EQ(embedder.dimension(), 384);
}

TEST(OnnxEmbedderTest, InitSuccess) {
    OnnxEmbedder embedder("", 128);
    Status s = embedder.Init();
    EXPECT_TRUE(s.ok());
}

// ============================================================
// Embed - null result pointer
// ============================================================

TEST(OnnxEmbedderTest, Embed_NullResult) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    Status s = embedder.Embed("hello", nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// ============================================================
// Embed - not initialized
// ============================================================

TEST(OnnxEmbedderTest, Embed_NotInitialized) {
    OnnxEmbedder embedder("", 128);
    // Do NOT call Init()
    EmbeddingResult result;
    Status s = embedder.Embed("hello", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("not initialized"), std::string::npos);
}

// ============================================================
// Embed - basic success
// ============================================================

TEST(OnnxEmbedderTest, Embed_BasicSuccess) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();

    EmbeddingResult result;
    Status s = embedder.Embed("hello world", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.dim, 128);
    EXPECT_EQ(static_cast<int>(result.vector.size()), 128);
    EXPECT_GT(result.inference_time_ms, 0.0f);
}

// ============================================================
// Embed - vector is unit-normalized
// ============================================================

TEST(OnnxEmbedderTest, Embed_UnitNormalized) {
    OnnxEmbedder embedder("", 256);
    embedder.Init();

    EmbeddingResult result;
    Status s = embedder.Embed("test normalization", &result);
    ASSERT_TRUE(s.ok());

    // Check L2 norm is approximately 1.0
    float norm = 0.0f;
    for (int i = 0; i < result.dim; ++i) {
        norm += result.vector[i] * result.vector[i];
    }
    norm = std::sqrt(norm);
    EXPECT_NEAR(norm, 1.0f, 0.001f);
}

// ============================================================
// Embed - deterministic for same input
// ============================================================

TEST(OnnxEmbedderTest, Embed_Deterministic) {
    OnnxEmbedder embedder("", 64);
    embedder.Init();

    EmbeddingResult r1, r2;
    embedder.Embed("deterministic test", &r1);
    embedder.Embed("deterministic test", &r2);

    ASSERT_EQ(r1.vector.size(), r2.vector.size());
    for (size_t i = 0; i < r1.vector.size(); ++i) {
        EXPECT_FLOAT_EQ(r1.vector[i], r2.vector[i]);
    }
}

// ============================================================
// Embed - different inputs produce different vectors
// ============================================================

TEST(OnnxEmbedderTest, Embed_DifferentInputsDifferentVectors) {
    OnnxEmbedder embedder("", 64);
    embedder.Init();

    EmbeddingResult r1, r2;
    embedder.Embed("first text", &r1);
    embedder.Embed("second text", &r2);

    bool all_same = true;
    for (size_t i = 0; i < r1.vector.size(); ++i) {
        if (r1.vector[i] != r2.vector[i]) {
            all_same = false;
            break;
        }
    }
    EXPECT_FALSE(all_same);
}

// ============================================================
// Embed - empty text
// ============================================================

TEST(OnnxEmbedderTest, Embed_EmptyText) {
    OnnxEmbedder embedder("", 64);
    embedder.Init();

    EmbeddingResult result;
    Status s = embedder.Embed("", &result);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(result.dim, 64);
    EXPECT_EQ(static_cast<int>(result.vector.size()), 64);
}

// ============================================================
// EmbedBatch - null results pointer
// ============================================================

TEST(OnnxEmbedderTest, EmbedBatch_NullResults) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();
    Status s = embedder.EmbedBatch({"hello"}, nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// ============================================================
// EmbedBatch - basic success
// ============================================================

TEST(OnnxEmbedderTest, EmbedBatch_BasicSuccess) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();

    std::vector<std::string> texts = {"hello", "world", "test"};
    std::vector<EmbeddingResult> results;
    Status s = embedder.EmbedBatch(texts, &results);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(results.size(), 3u);
    for (const auto& r : results) {
        EXPECT_EQ(r.dim, 128);
        EXPECT_EQ(static_cast<int>(r.vector.size()), 128);
    }
}

// ============================================================
// EmbedBatch - empty list
// ============================================================

TEST(OnnxEmbedderTest, EmbedBatch_EmptyList) {
    OnnxEmbedder embedder("", 128);
    embedder.Init();

    std::vector<std::string> texts;
    std::vector<EmbeddingResult> results;
    Status s = embedder.EmbedBatch(texts, &results);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(results.empty());
}

// ============================================================
// EmbedBatch - single item
// ============================================================

TEST(OnnxEmbedderTest, EmbedBatch_SingleItem) {
    OnnxEmbedder embedder("", 64);
    embedder.Init();

    std::vector<std::string> texts = {"single"};
    std::vector<EmbeddingResult> results;
    Status s = embedder.EmbedBatch(texts, &results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].dim, 64);
}

// ============================================================
// EmbedBatch - not initialized
// ============================================================

TEST(OnnxEmbedderTest, EmbedBatch_NotInitialized) {
    OnnxEmbedder embedder("", 128);
    // Do NOT call Init()

    std::vector<std::string> texts = {"hello"};
    std::vector<EmbeddingResult> results;
    Status s = embedder.EmbedBatch(texts, &results);
    EXPECT_FALSE(s.ok());
}

// ============================================================
// Dimension accessor
// ============================================================

TEST(OnnxEmbedderTest, Dimension_SmallDim) {
    OnnxEmbedder embedder("", 4);
    EXPECT_EQ(embedder.dimension(), 4);
}

TEST(OnnxEmbedderTest, Dimension_1024) {
    OnnxEmbedder embedder("");
    EXPECT_EQ(embedder.dimension(), 1024);
}

// ============================================================
// Stub mode tests (new in F03-ONNX)
// ============================================================

TEST(OnnxEmbedderTest, StubMode_NoModel) {
    // With empty model path, should init in stub mode
    OnnxEmbedder embedder("", 128);
    ASSERT_TRUE(embedder.Init().ok());
    EXPECT_FALSE(embedder.is_real_model());
}

TEST(OnnxEmbedderTest, StubMode_NonexistentModel) {
    // With non-existent model path, should fallback to stub mode
    OnnxEmbedder embedder("/nonexistent/path/model.onnx", 128);
    ASSERT_TRUE(embedder.Init().ok());
    EXPECT_FALSE(embedder.is_real_model());
}

TEST(OnnxEmbedderTest, SetTokenizerPath) {
    OnnxEmbedder embedder("", 128);
    embedder.set_tokenizer_path("/some/tokenizer.json");
    ASSERT_TRUE(embedder.Init().ok());
    // Should still be stub mode (no actual model)
    EXPECT_FALSE(embedder.is_real_model());
}

TEST(OnnxEmbedderTest, SetMaxSeqLength) {
    OnnxEmbedder embedder("", 128);
    embedder.set_max_seq_length(256);
    ASSERT_TRUE(embedder.Init().ok());
    EXPECT_FALSE(embedder.is_real_model());
}

TEST(OnnxEmbedderTest, StubEmbed_StillWorks) {
    // Verify stub mode still produces valid embeddings
    OnnxEmbedder embedder("/no/such/model.onnx", 64);
    ASSERT_TRUE(embedder.Init().ok());
    EXPECT_FALSE(embedder.is_real_model());

    EmbeddingResult result;
    Status s = embedder.Embed("test text", &result);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(result.dim, 64);
    EXPECT_EQ(static_cast<int>(result.vector.size()), 64);

    // Should still be unit-normalized
    float norm = 0.0f;
    for (int i = 0; i < result.dim; ++i) {
        norm += result.vector[i] * result.vector[i];
    }
    norm = std::sqrt(norm);
    EXPECT_NEAR(norm, 1.0f, 0.001f);
}

}  // namespace
}  // namespace cortrix
