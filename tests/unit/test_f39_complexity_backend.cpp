#include <gtest/gtest.h>

#include "cortrix/query/complexity_config.h"
#include "cortrix/query/heuristic_complexity_backend.h"

// Query routing coverage: the standalone HeuristicComplexityBackend (no-ONNX stub) + the
// ComplexityConfig validation helpers (§4.2).
namespace cortrix::query {
namespace {

TEST(F39HeuristicBackendTest, Identity) {
    HeuristicComplexityBackend b;
    EXPECT_EQ(b.Name(), "heuristic_guard");
    EXPECT_TRUE(b.IsAvailable());
}

TEST(F39HeuristicBackendTest, ShortQueryIsSimple) {
    HeuristicComplexityBackend b;
    auto r = b.Infer("Cortrix overview");
    EXPECT_EQ(r.label, "simple");
    EXPECT_NEAR(r.confidence, 0.9f, 1e-6);
}

TEST(F39HeuristicBackendTest, ComparisonCueIsComplex) {
    HeuristicComplexityBackend b;
    EXPECT_EQ(b.Infer("compare A and B").label, "complex");
    EXPECT_EQ(b.Infer("X vs Y").label, "complex");
    EXPECT_EQ(b.Infer("what is the difference between dense and sparse retrieval").label,
              "complex");
}

TEST(F39HeuristicBackendTest, LongQueryIsComplex) {
    HeuristicComplexityBackend b;
    // >= 12 whitespace tokens → complex (multi-step intuition).
    auto r = b.Infer("one two three four five six seven eight nine ten eleven twelve");
    EXPECT_EQ(r.label, "complex");
}

TEST(F39HeuristicBackendTest, EmptyQueryIsChatSafetyNet) {
    HeuristicComplexityBackend b;
    EXPECT_EQ(b.Infer("").label, "chat");
    EXPECT_EQ(b.Infer("   ").label, "chat");
}

TEST(F39HeuristicBackendTest, ChineseComparisonCueIsComplex) {
    HeuristicComplexityBackend b;
    // Chinese "compare A and B" query — contains compare/difference cues (bytes below).
    EXPECT_EQ(b.Infer("\xe5\xaf\xb9\xe6\xaf\x94 A \xe5\x92\x8c B "
                      "\xe7\x9a\x84\xe5\x8c\xba\xe5\x88\xab")
                  .label,
              "complex");
}

TEST(F39HeuristicBackendTest, ReportedConfidenceConfigurable) {
    HeuristicComplexityBackend b(0.3f);
    EXPECT_NEAR(b.Infer("anything short").confidence, 0.3f, 1e-6);
}

// --- ComplexityConfig (§4.2) --------------------------------------------------

TEST(F39ComplexityConfigTest, DefaultsMatchSpec) {
    ComplexityConfig c;
    EXPECT_TRUE(c.enabled);
    EXPECT_EQ(c.force_route, "auto");
    EXPECT_NEAR(c.confidence_threshold, 0.5f, 1e-6);
    EXPECT_EQ(c.evaluation_method, "small_classifier");
    EXPECT_TRUE(c.fallback_to_complex_on_failure);
    EXPECT_TRUE(c.multi_turn_warning_enabled);
    EXPECT_EQ(c.max_inference_retries, 3);
    EXPECT_TRUE(c.IsThresholdValid());
    EXPECT_TRUE(c.IsForceRouteValid());
}

TEST(F39ComplexityConfigTest, ThresholdValidationBounds) {
    ComplexityConfig c;
    c.confidence_threshold = 0.3f;
    EXPECT_TRUE(c.IsThresholdValid());
    c.confidence_threshold = 0.8f;
    EXPECT_TRUE(c.IsThresholdValid());
    c.confidence_threshold = 0.29f;
    EXPECT_FALSE(c.IsThresholdValid());
    c.confidence_threshold = 0.81f;
    EXPECT_FALSE(c.IsThresholdValid());
}

TEST(F39ComplexityConfigTest, ForceRouteValidation) {
    ComplexityConfig c;
    for (const char* ok : {"auto", "simple", "complex", "chat"}) {
        c.force_route = ok;
        EXPECT_TRUE(c.IsForceRouteValid()) << ok;
    }
    c.force_route = "bogus";
    EXPECT_FALSE(c.IsForceRouteValid());
    c.force_route = "";
    EXPECT_FALSE(c.IsForceRouteValid());
}

}  // namespace
}  // namespace cortrix::query
