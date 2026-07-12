#include <gtest/gtest.h>
#include "cortrix/reranker/onnx_reranker.h"
#include <filesystem>
#include <iostream>

namespace cortrix::reranker {
namespace {

namespace fs = std::filesystem;

// F02 reranker real cross-encoder inference (bge-reranker-v2-m3 ONNX).
// SKIPPED if model files are not present on disk. To run: convert the model
// with scripts/convert_reranker_to_onnx.py into models/bge-reranker-v2-m3/.

static std::string FindRerankerModelDir() {
    std::vector<std::string> candidates = {
        "models/bge-reranker-v2-m3",
        "../models/bge-reranker-v2-m3",
        "../../models/bge-reranker-v2-m3",
        "../../../models/bge-reranker-v2-m3",
    };
    const char* src_dir = std::getenv("CORTRIX_SOURCE_DIR");
    if (src_dir) {
        candidates.push_back(std::string(src_dir) + "/models/bge-reranker-v2-m3");
    }
    for (const auto& dir : candidates) {
        if (fs::exists(dir + "/model.onnx") && fs::exists(dir + "/tokenizer.json")) {
            return dir;
        }
    }
    return "";
}

class RerankerRealInferenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string dir = FindRerankerModelDir();
        if (dir.empty()) {
            GTEST_SKIP() << "bge-reranker-v2-m3 model not found, skipping real "
                            "inference test. Convert with "
                            "scripts/convert_reranker_to_onnx.py into "
                            "models/bge-reranker-v2-m3/";
        }
        config_.model_path = dir + "/model.onnx";
        config_.tokenizer_path = dir + "/tokenizer.json";
        // CPU EP: deterministic across machines; CoreML auto is covered by unit
        // tests and would only change the EP, not the contract under test.
        config_.execution_provider = "cpu";
    }

    RerankerConfig config_;
};

TEST_F(RerankerRealInferenceTest, InitLoadsRealModel) {
    OnnxReranker reranker(config_, /*chunk_store=*/nullptr);
    Status s = reranker.Init();
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(reranker.is_real_model());
    EXPECT_TRUE(reranker.is_ready());
}

TEST_F(RerankerRealInferenceTest, RelevantPassageScoresHigher) {
    OnnxReranker reranker(config_, nullptr);
    ASSERT_TRUE(reranker.Init().ok());

    const char* query = "What is the capital of France?";
    float relevant = reranker.Score(query, "Paris is the capital and largest city of France.");
    float irrelevant = reranker.Score(query, "Photosynthesis converts sunlight into chemical energy in plants.");

    std::cout << "[RERANK] relevant=" << relevant << " irrelevant=" << irrelevant << std::endl;

    // Scores are sigmoid outputs in [0,1] (score_fusion.h contract).
    EXPECT_GE(relevant, 0.0f);
    EXPECT_LE(relevant, 1.0f);
    EXPECT_GE(irrelevant, 0.0f);
    EXPECT_LE(irrelevant, 1.0f);
    // The cross-encoder must rank the on-topic passage clearly higher.
    EXPECT_GT(relevant, irrelevant);
    EXPECT_GT(relevant, 0.5f) << "on-topic passage should score above midpoint";
    EXPECT_LT(irrelevant, 0.5f) << "off-topic passage should score below midpoint";
}

TEST_F(RerankerRealInferenceTest, ScoreBatchMatchesSingleScore) {
    OnnxReranker reranker(config_, nullptr);
    ASSERT_TRUE(reranker.Init().ok());

    const char* query = "machine learning frameworks";
    std::vector<const char*> passages = {
        "PyTorch and TensorFlow are widely used machine learning frameworks.",
        "The recipe calls for two cups of flour and one egg.",
        "Scikit-learn provides classical machine learning algorithms in Python.",
    };

    std::vector<float> batch = reranker.ScoreBatch(query, passages);
    ASSERT_EQ(batch.size(), passages.size());

    for (size_t i = 0; i < passages.size(); i++) {
        float single = reranker.Score(query, passages[i]);
        // Same pair through the pool path and the direct path must agree
        // (identical encoding + session; CPU EP is deterministic).
        EXPECT_NEAR(batch[i], single, 1e-4f) << "passage " << i;
    }
    EXPECT_GT(batch[0], batch[1]);
    EXPECT_GT(batch[2], batch[1]);
}

}  // namespace
}  // namespace cortrix::reranker
