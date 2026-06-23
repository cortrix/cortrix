// R7 plan-B real-inference coverage for src/spc/onnx_embedder.cpp (47.9%).
//
// The real OnnxEmbedder::Init (real Ort::Session create + HfTokenizer load) and the
// real Embed inference path (tokenize → session->Run → CLS extract → L2 normalize)
// are ONLY exercised by tests/integration/test_onnx_real_inference.cpp, which is
// (a) integration-label and (b) excluded from the `-L unit` coverage gate — so the
// embedder's real path is at 0% in the unit coverage metric (the file sits at 47.9%,
// the rest being the stub path). This is a unit-label MIRROR that runs the SAME real
// path against the on-disk models/bge-m3 model, so the real Init+Embed+EmbedBatch
// (and the F40 EmbedWithSparse dense half) land in the unit coverage gate.
//
// Self-skips (GTEST_SKIP) when the model is absent, so a model-less CI box / box
// without the 2.27 GB bge-m3 weights stays green. Standalone NEW file; does not
// touch the integration test.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/spc/onnx_embedder.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;

// Resolve models/bge-m3 robustly across the possible ctest working directories:
//   1. the CORTRIX_SOURCE_DIR compile macro (when the build defines it for this
//      target — same knob the integration build uses, just as a -D here);
//   2. the CORTRIX_SOURCE_DIR environment variable (set by coverage.sh / ctest);
//   3. relative candidates from the test binary's cwd (build*/ → ../models).
// Requires model.onnx + tokenizer.json present (model.onnx_data is loaded
// implicitly by ORT alongside model.onnx for the >2 GB external-data bge-m3 export).
static std::string FindModelDir() {
    std::vector<std::string> candidates;
#ifdef CORTRIX_SOURCE_DIR
    candidates.push_back(std::string(CORTRIX_SOURCE_DIR) + "/models/bge-m3");
#endif
    if (const char* src_dir = std::getenv("CORTRIX_SOURCE_DIR")) {
        candidates.push_back(std::string(src_dir) + "/models/bge-m3");
    }
    candidates.insert(candidates.end(), {
        "models/bge-m3",
        "../models/bge-m3",
        "../../models/bge-m3",
        "../../../models/bge-m3",
    });
    for (const auto& dir : candidates) {
        if (fs::exists(dir + "/model.onnx") && fs::exists(dir + "/tokenizer.json")) {
            return dir;
        }
    }
    return "";
}

static float CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    return denom > 0 ? dot / denom : 0.0f;
}

class OnnxEmbedderRealTest : public ::testing::Test {
protected:
    void SetUp() override {
        model_dir_ = FindModelDir();
        if (model_dir_.empty()) {
            GTEST_SKIP() << "bge-m3 model not found (models/bge-m3/model.onnx + "
                            "tokenizer.json); skipping real-inference unit test.";
        }
        model_path_ = model_dir_ + "/model.onnx";
        tokenizer_path_ = model_dir_ + "/tokenizer.json";
    }

    // A real-model embedder (CPU/auto EP). dim=1024 = bge-m3 hidden size.
    std::unique_ptr<OnnxEmbedder> MakeRealEmbedder() {
        auto e = std::make_unique<OnnxEmbedder>(model_path_, 1024);
        e->set_tokenizer_path(tokenizer_path_);
        return e;
    }

    std::string model_dir_, model_path_, tokenizer_path_;
};

// Real Init: loads the HfTokenizer + creates a real Ort::Session (the whole
// non-stub branch of Init), then is_real_model() flips true.
TEST_F(OnnxEmbedderRealTest, InitLoadsRealModel) {
    auto e = MakeRealEmbedder();
    Status s = e->Init();
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(e->is_real_model());
    EXPECT_EQ(e->dimension(), 1024);
    // is_coreml_active() is platform-dependent (true on Apple when the CoreML EP
    // engaged); just confirm the accessor is callable post-Init (no assertion on
    // the value — it reflects the machine, not the contract).
    (void)e->is_coreml_active();
}

// Real Embed: tokenize → session->Run → CLS-token extract → L2 normalize. Asserts
// the 1024-dim unit-normalized vector + a measured inference time.
TEST_F(OnnxEmbedderRealTest, EmbedRealProducesUnitNorm1024) {
    auto e = MakeRealEmbedder();
    ASSERT_TRUE(e->Init().ok());

    EmbeddingResult r;
    Status s = e->Embed("Hello world", &r);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(r.dim, 1024);
    EXPECT_EQ(static_cast<int>(r.vector.size()), 1024);
    EXPECT_GT(r.inference_time_ms, 0.0f);

    float norm = 0.0f;
    for (float v : r.vector) norm += v * v;
    EXPECT_NEAR(std::sqrt(norm), 1.0f, 0.01f);
}

// Real Embed is deterministic (same input → identical vector through the real
// session). Guards the CLS-extract + normalize path's stability.
TEST_F(OnnxEmbedderRealTest, EmbedRealDeterministic) {
    auto e = MakeRealEmbedder();
    ASSERT_TRUE(e->Init().ok());

    EmbeddingResult r1, r2;
    ASSERT_TRUE(e->Embed("determinism", &r1).ok());
    ASSERT_TRUE(e->Embed("determinism", &r2).ok());
    ASSERT_EQ(r1.vector.size(), r2.vector.size());
    for (size_t i = 0; i < r1.vector.size(); ++i)
        EXPECT_FLOAT_EQ(r1.vector[i], r2.vector[i]) << "at " << i;
}

// Real semantics: related sentences score higher cosine than unrelated. Exercises
// the real inference path on several distinct inputs (multiple session->Run calls).
TEST_F(OnnxEmbedderRealTest, EmbedRealSemanticOrdering) {
    auto e = MakeRealEmbedder();
    ASSERT_TRUE(e->Init().ok());

    EmbeddingResult a, b, c;
    ASSERT_TRUE(e->Embed("The cat sat on the mat", &a).ok());
    ASSERT_TRUE(e->Embed("A cat was sitting on a mat", &b).ok());
    ASSERT_TRUE(e->Embed("Quarterly revenue rose on strong cloud demand", &c).ok());
    EXPECT_GT(CosineSimilarity(a.vector, b.vector),
              CosineSimilarity(a.vector, c.vector));
}

// Real EmbedBatch: the sequential per-item real-inference loop (one session->Run
// per text). Each result carries the 1024-dim vector + a measured time.
TEST_F(OnnxEmbedderRealTest, EmbedBatchRealAllDims) {
    auto e = MakeRealEmbedder();
    ASSERT_TRUE(e->Init().ok());

    std::vector<std::string> texts = {"alpha", "beta", "gamma"};
    std::vector<EmbeddingResult> results;
    Status s = e->EmbedBatch(texts, &results);
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_EQ(results.size(), 3u);
    for (const auto& r : results) {
        EXPECT_EQ(r.dim, 1024);
        EXPECT_EQ(static_cast<int>(r.vector.size()), 1024);
        EXPECT_GT(r.inference_time_ms, 0.0f);
    }
}

// F40 EmbedWithSparse over the REAL model: the dense half runs the real inference
// path (1024-dim unit vector), the sparse half is the standalone stub (D3) keyed off
// the same text. Exercises the EmbedWithSparse dense=real branch + sparse stub on a
// real session (the dense path is the part that was uncovered in the unit metric).
TEST_F(OnnxEmbedderRealTest, EmbedWithSparseRealDenseHalf) {
    auto e = MakeRealEmbedder();
    ASSERT_TRUE(e->Init().ok());

    EmbedWithSparseResult r;
    Status s = e->EmbedWithSparse("machine learning frameworks", &r, /*top_k=*/50);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(static_cast<int>(r.dense.size()), 1024);  // real dense embedding
    EXPECT_GT(r.inference_time_ms, 0.0f);
    // sparse is the deterministic stub (non-empty for multi-word input), top-K capped.
    EXPECT_FALSE(r.sparse.empty());
    EXPECT_LE(static_cast<int>(r.sparse.size()), 50);
}

// F40 EmbedBatchWithSparse over the real model — the batch form of the above
// (sequential per-item real dense + stub sparse).
TEST_F(OnnxEmbedderRealTest, EmbedBatchWithSparseReal) {
    auto e = MakeRealEmbedder();
    ASSERT_TRUE(e->Init().ok());

    std::vector<std::string> texts = {"vector database", "semantic search"};
    std::vector<EmbedWithSparseResult> results;
    Status s = e->EmbedBatchWithSparse(texts, &results, /*top_k=*/100);
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_EQ(results.size(), 2u);
    for (const auto& r : results) {
        EXPECT_EQ(static_cast<int>(r.dense.size()), 1024);
    }
}

}  // namespace
}  // namespace cortrix
