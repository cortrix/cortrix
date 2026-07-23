#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "cortrix/query/onnx_complexity_backend.h"
#include "cortrix/query/router_error.h"

// OnnxComplexityBackend coverage. The real-inference smoke cases reproduce
// models/_dl/train/smoke_onnx.py (the 5 samples recorded in the model README's
// "Smoke test" table). They run only when the 256 MB model archive is present AND
// this build linked ONNX Runtime; otherwise GTEST_SKIP (CI / offline friendly),
// matching the in-tree ONNX test convention.

namespace cortrix::query {
namespace {

namespace fs = std::filesystem;

const std::string kModelDir =
    std::string(CORTRIX_CE_SOURCE_DIR) + "/models/query-complexity";

std::string ModelPath() { return kModelDir + "/model.onnx"; }

bool ModelPresent() { return fs::exists(ModelPath()); }

// --- Offline behavior: no model file → backend constructible + unavailable. ---

TEST(OnnxComplexityBackendTest, Identity) {
    OnnxComplexityBackend b("/nonexistent/model.onnx");
    EXPECT_EQ(b.Name(), "onnx_distilbert");
}

TEST(OnnxComplexityBackendTest, MissingModelIsUnavailableNotError) {
    OnnxComplexityBackend b("/nonexistent/model.onnx");
    // Init() succeeds (offline-friendly) but the backend stays unavailable so
    // QueryComplexityClassifier takes the F39 L2 default-Complex path.
    EXPECT_TRUE(b.Init().ok());
    EXPECT_FALSE(b.IsAvailable());
}

TEST(OnnxComplexityBackendTest, InferWhileUnavailableThrows) {
    OnnxComplexityBackend b("/nonexistent/model.onnx");
    ASSERT_TRUE(b.Init().ok());
    ASSERT_FALSE(b.IsAvailable());
    // Misuse (the classifier never does this — it checks IsAvailable() first) is
    // surfaced on the transient retry/degrade path.
    EXPECT_THROW(b.Infer("hello"), RouterInferenceError);
}

// --- Real inference smoke: README "Smoke test" 5 samples. ---

struct SmokeCase {
    const char* text;
    const char* expected_label;  // README pred column
};

TEST(OnnxComplexityBackendTest, SmokeParityWithReadme) {
    if (!ModelPresent()) {
        GTEST_SKIP() << "query-complexity model.onnx absent (offline build)";
    }
    OnnxComplexityBackend backend(ModelPath());
    Status init = backend.Init();
    if (!backend.IsAvailable()) {
        // Model present but this build did not link ONNX Runtime → can't infer.
        GTEST_SKIP() << "ONNX Runtime not linked / model unloadable: " << init.message();
    }

    // The 5 smoke samples are the VERBATIM SAMPLES list from
    // models/_dl/train/smoke_onnx.py (the README table abbreviates them with "…").
    // Their predictions are that script's recorded output. The README flags two as
    // a documented domain-shift limitation (the QA-benchmark labels make
    // "Hello, thanks!"→simple and "What is Cortrix?"→chat) — we assert the model's
    // ACTUAL output, not the intuitive label, so this is a true parity check on the
    // exported model + our tokenization, not a quality gate.
    const SmokeCase cases[] = {
        {"Hello, thanks!", "simple"},
        {"What is Cortrix?", "chat"},
        {"When was the company that makes Cortrix founded?", "complex"},
        {"Compare the retrieval precision of Cortrix vs Mem0 vs RAGFlow in "
         "enterprise scenarios and explain why they differ", "simple"},
        {"Which director of the 2024 film also directed the sequel released the "
         "following year?", "complex"},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.text);
        ComplexityBackendResult r = backend.Infer(c.text);
        EXPECT_EQ(r.label, c.expected_label);
        // softmax (in-graph) → confidence in (0,1]; README confidences are ≥0.92.
        EXPECT_GT(r.confidence, 0.5f);
        EXPECT_LE(r.confidence, 1.0001f);
    }
}

TEST(OnnxComplexityBackendTest, InitIsIdempotent) {
    if (!ModelPresent()) GTEST_SKIP() << "model absent";
    OnnxComplexityBackend backend(ModelPath());
    ASSERT_TRUE(backend.Init().ok());
    if (!backend.IsAvailable()) GTEST_SKIP() << "ONNX not linked";
    // A second Init() is a no-op and keeps the backend ready.
    EXPECT_TRUE(backend.Init().ok());
    EXPECT_TRUE(backend.IsAvailable());
}

}  // namespace
}  // namespace cortrix::query
