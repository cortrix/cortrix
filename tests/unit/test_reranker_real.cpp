// R7 plan-B real-inference coverage for src/reranker/onnx_reranker.cpp.
//
// The real OnnxReranker::Init (CoreML/CPU EP selection + real Ort::Session create +
// HfTokenizer load) and the real Score/ScoreBatch cross-encoder inference
// (RunCrossEncoder: tokenize query+passage → session->Run → sigmoid) are ONLY
// exercised by tests/integration/test_reranker_real_inference.cpp, which is
// integration-label and excluded from the `-L unit` coverage gate — so the
// reranker's real path is at 0% in the unit coverage metric. This is a unit-label
// MIRROR that runs the SAME real path against models/bge-reranker-v2-m3, so real
// Init+Score+ScoreBatch land in the unit gate. Adds a default-EP (auto) case so the
// CoreML-vs-CPU EP selection branch is covered on Apple boxes.
//
// Self-skips (GTEST_SKIP) when the model is absent. Standalone NEW file; does not
// touch the integration test.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/reranker/onnx_reranker.h"
#include "cortrix/reranker/reranker_config.h"

namespace cortrix::reranker {
namespace {

namespace fs = std::filesystem;

// Resolve models/bge-reranker-v2-m3 across possible ctest cwds (compile macro / env
// / relative candidates) — mirrors test_onnx_embedder_real.cpp's resolver.
static std::string FindRerankerModelDir() {
    std::vector<std::string> candidates;
#ifdef CORTRIX_SOURCE_DIR
    candidates.push_back(std::string(CORTRIX_SOURCE_DIR) + "/models/bge-reranker-v2-m3");
#endif
    if (const char* src_dir = std::getenv("CORTRIX_SOURCE_DIR")) {
        candidates.push_back(std::string(src_dir) + "/models/bge-reranker-v2-m3");
    }
    candidates.insert(candidates.end(), {
        "models/bge-reranker-v2-m3",
        "../models/bge-reranker-v2-m3",
        "../../models/bge-reranker-v2-m3",
        "../../../models/bge-reranker-v2-m3",
    });
    for (const auto& dir : candidates) {
        if (fs::exists(dir + "/model.onnx") && fs::exists(dir + "/tokenizer.json")) {
            return dir;
        }
    }
    return "";
}

class RerankerRealTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = FindRerankerModelDir();
        if (dir_.empty()) {
            GTEST_SKIP() << "bge-reranker-v2-m3 model not found "
                            "(models/bge-reranker-v2-m3/model.onnx + tokenizer.json); "
                            "skipping real-inference unit test.";
        }
    }

    RerankerConfig MakeConfig(RerankerConfig::UseCoreML ep) {
        RerankerConfig c;
        c.model_path = dir_ + "/model.onnx";
        c.tokenizer_path = dir_ + "/tokenizer.json";
        c.use_coreml = ep;
        return c;
    }

    std::string dir_;
};

// Real Init (CPU EP forced for deterministic numbers): HfTokenizer load + real
// Ort::Session create. is_real_model()/is_ready() flip true; active_ep()=="cpu".
TEST_F(RerankerRealTest, InitLoadsRealModelCpu) {
    OnnxReranker r(MakeConfig(RerankerConfig::UseCoreML::kForceFalse), /*chunk_store=*/nullptr);
    Status s = r.Init();
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(r.is_real_model());
    EXPECT_TRUE(r.is_ready());
    EXPECT_STREQ(r.active_ep(), "cpu");
    EXPECT_FALSE(r.is_coreml_active());
}

// Real Init with the DEFAULT (auto) EP — exercises the CoreML-vs-CPU EP selection
// branch in Init (on Apple the CoreML EP engages; elsewhere it falls back to CPU).
// Asserts only that Init succeeds + the model is real (the active EP is
// platform-dependent, so no fixed assertion on it).
TEST_F(RerankerRealTest, InitLoadsRealModelAutoEp) {
    OnnxReranker r(MakeConfig(RerankerConfig::UseCoreML::kAuto), nullptr);
    Status s = r.Init();
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(r.is_real_model());
    EXPECT_TRUE(r.is_ready());
    // active_ep() is "coreml" or "cpu" depending on the box — just confirm callable.
    const char* ep = r.active_ep();
    EXPECT_TRUE(std::string(ep) == "coreml" || std::string(ep) == "cpu");
}

// Real Score (RunCrossEncoder): tokenize query+passage → session->Run → sigmoid in
// [0,1]. The on-topic passage must score clearly above the off-topic one.
TEST_F(RerankerRealTest, ScoreRealRanksRelevantHigher) {
    OnnxReranker r(MakeConfig(RerankerConfig::UseCoreML::kForceFalse), nullptr);
    ASSERT_TRUE(r.Init().ok());

    const char* query = "What is the capital of France?";
    float relevant = r.Score(query, "Paris is the capital and largest city of France.");
    float irrelevant =
        r.Score(query, "Photosynthesis converts sunlight into chemical energy in plants.");

    EXPECT_GE(relevant, 0.0f);
    EXPECT_LE(relevant, 1.0f);
    EXPECT_GE(irrelevant, 0.0f);
    EXPECT_LE(irrelevant, 1.0f);
    EXPECT_GT(relevant, irrelevant);
    EXPECT_GT(relevant, 0.5f);
    EXPECT_LT(irrelevant, 0.5f);
}

// Real ScoreBatch (the thread-pool per-passage path): each pair through the pool
// must agree with the direct Score() (same encoding + session, CPU deterministic).
TEST_F(RerankerRealTest, ScoreBatchRealMatchesSingle) {
    OnnxReranker r(MakeConfig(RerankerConfig::UseCoreML::kForceFalse), nullptr);
    ASSERT_TRUE(r.Init().ok());

    const char* query = "machine learning frameworks";
    std::vector<const char*> passages = {
        "PyTorch and TensorFlow are widely used machine learning frameworks.",
        "The recipe calls for two cups of flour and one egg.",
        "Scikit-learn provides classical machine learning algorithms in Python.",
    };
    std::vector<float> batch = r.ScoreBatch(query, passages);
    ASSERT_EQ(batch.size(), passages.size());
    for (size_t i = 0; i < passages.size(); ++i) {
        float single = r.Score(query, passages[i]);
        EXPECT_NEAR(batch[i], single, 1e-4f) << "passage " << i;
    }
    EXPECT_GT(batch[0], batch[1]);
    EXPECT_GT(batch[2], batch[1]);
}

// Real ScoreBatch with a single passage (degenerate batch — still goes through the
// pool path) and an empty passage list (boundary: empty result, no inference).
TEST_F(RerankerRealTest, ScoreBatchRealBoundaries) {
    OnnxReranker r(MakeConfig(RerankerConfig::UseCoreML::kForceFalse), nullptr);
    ASSERT_TRUE(r.Init().ok());

    std::vector<const char*> one = {"Paris is the capital of France."};
    std::vector<float> b1 = r.ScoreBatch("capital of France", one);
    ASSERT_EQ(b1.size(), 1u);
    EXPECT_GE(b1[0], 0.0f);
    EXPECT_LE(b1[0], 1.0f);

    std::vector<const char*> none;
    std::vector<float> b0 = r.ScoreBatch("anything", none);
    EXPECT_TRUE(b0.empty());
}

}  // namespace
}  // namespace cortrix::reranker
