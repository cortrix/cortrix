#include <gtest/gtest.h>
#include "cortrix/spc/onnx_embedder.h"
#include <cmath>
#include <filesystem>
#include <iostream>

namespace cortrix {
namespace {

namespace fs = std::filesystem;

// This integration test verifies real ONNX Runtime inference with bge-m3.
// It is SKIPPED if model files are not present on disk.
// To run: download model.onnx and tokenizer.json to models/bge-m3/ directory.

static std::string FindModelDir() {
    // Search for models/bge-m3/ relative to the test binary
    // Try a few common locations
    std::vector<std::string> candidates = {
        "models/bge-m3",
        "../models/bge-m3",
        "../../models/bge-m3",
        "../../../models/bge-m3",
    };
    // Also check from SOURCE_DIR if available
    const char* src_dir = std::getenv("CORTRIX_SOURCE_DIR");
    if (src_dir) {
        candidates.push_back(std::string(src_dir) + "/models/bge-m3");
    }
    for (const auto& dir : candidates) {
        std::string model_path = dir + "/model.onnx";
        std::string tok_path = dir + "/tokenizer.json";
        if (fs::exists(model_path) && fs::exists(tok_path)) {
            return dir;
        }
    }
    return "";
}

class OnnxRealInferenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        model_dir_ = FindModelDir();
        if (model_dir_.empty()) {
            GTEST_SKIP() << "bge-m3 model not found, skipping real inference test. "
                         << "Download model.onnx and tokenizer.json to models/bge-m3/";
        }
        model_path_ = model_dir_ + "/model.onnx";
        tokenizer_path_ = model_dir_ + "/tokenizer.json";
    }

    std::string model_dir_;
    std::string model_path_;
    std::string tokenizer_path_;
};

float CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    return denom > 0 ? dot / denom : 0.0f;
}

TEST_F(OnnxRealInferenceTest, InitWithRealModel) {
    OnnxEmbedder embedder(model_path_, 1024);
    embedder.set_tokenizer_path(tokenizer_path_);

    Status s = embedder.Init();
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(embedder.is_real_model());
}

TEST_F(OnnxRealInferenceTest, EmbedProduces1024DimVector) {
    OnnxEmbedder embedder(model_path_, 1024);
    embedder.set_tokenizer_path(tokenizer_path_);
    ASSERT_TRUE(embedder.Init().ok());

    EmbeddingResult result;
    Status s = embedder.Embed("Hello world", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.dim, 1024);
    EXPECT_EQ(static_cast<int>(result.vector.size()), 1024);
    EXPECT_GT(result.inference_time_ms, 0.0f);

    // Should be unit-normalized
    float norm = 0.0f;
    for (float v : result.vector) norm += v * v;
    norm = std::sqrt(norm);
    EXPECT_NEAR(norm, 1.0f, 0.01f);
}

TEST_F(OnnxRealInferenceTest, SimilarTextsSimilarVectors) {
    OnnxEmbedder embedder(model_path_, 1024);
    embedder.set_tokenizer_path(tokenizer_path_);
    ASSERT_TRUE(embedder.Init().ok());

    EmbeddingResult r1, r2, r3;
    ASSERT_TRUE(embedder.Embed("The cat sat on the mat", &r1).ok());
    ASSERT_TRUE(embedder.Embed("A cat was sitting on a mat", &r2).ok());
    ASSERT_TRUE(embedder.Embed("Stock market analysis for Q4 2025", &r3).ok());

    float sim_similar = CosineSimilarity(r1.vector, r2.vector);
    float sim_different = CosineSimilarity(r1.vector, r3.vector);

    std::cout << "[SEMANTIC] Similar texts cosine: " << sim_similar << std::endl;
    std::cout << "[SEMANTIC] Different texts cosine: " << sim_different << std::endl;

    // Similar sentences should have high cosine similarity
    EXPECT_GT(sim_similar, 0.7f) << "Similar sentences should have cosine > 0.7";
    // Unrelated sentences should have lower similarity
    EXPECT_LT(sim_different, sim_similar) << "Unrelated text should be less similar";
}

TEST_F(OnnxRealInferenceTest, MultilingualEmbedding) {
    OnnxEmbedder embedder(model_path_, 1024);
    embedder.set_tokenizer_path(tokenizer_path_);
    ASSERT_TRUE(embedder.Init().ok());

    EmbeddingResult r_en, r_zh;
    ASSERT_TRUE(embedder.Embed("machine learning algorithms", &r_en).ok());
    ASSERT_TRUE(embedder.Embed("机器学习算法", &r_zh).ok());

    float sim = CosineSimilarity(r_en.vector, r_zh.vector);
    std::cout << "[MULTILINGUAL] EN-ZH cosine similarity: " << sim << std::endl;

    // bge-m3 is multilingual, same concept in different languages should be similar
    EXPECT_GT(sim, 0.5f) << "Multilingual embeddings of same concept should be similar";
}

TEST_F(OnnxRealInferenceTest, BatchEmbedding) {
    OnnxEmbedder embedder(model_path_, 1024);
    embedder.set_tokenizer_path(tokenizer_path_);
    ASSERT_TRUE(embedder.Init().ok());

    std::vector<std::string> texts = {"hello", "world", "test"};
    std::vector<EmbeddingResult> results;
    Status s = embedder.EmbedBatch(texts, &results);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(results.size(), 3u);
    for (const auto& r : results) {
        EXPECT_EQ(r.dim, 1024);
        EXPECT_GT(r.inference_time_ms, 0.0f);
    }
}

TEST_F(OnnxRealInferenceTest, Deterministic) {
    OnnxEmbedder embedder(model_path_, 1024);
    embedder.set_tokenizer_path(tokenizer_path_);
    ASSERT_TRUE(embedder.Init().ok());

    EmbeddingResult r1, r2;
    ASSERT_TRUE(embedder.Embed("determinism test", &r1).ok());
    ASSERT_TRUE(embedder.Embed("determinism test", &r2).ok());

    for (size_t i = 0; i < r1.vector.size(); i++) {
        EXPECT_FLOAT_EQ(r1.vector[i], r2.vector[i]) << "at index " << i;
    }
}

}  // namespace
}  // namespace cortrix
