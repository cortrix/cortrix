// R7-1d branch-coverage supplement for src/query/rag_fusion_types.cpp.
//
// The large test_rag_fusion.cpp suite drives QueryVariantGenerator / RRF fusion /
// the ConfigResolver three-layer merge, but never calls the standalone
// rag_fusion_types.cpp free functions directly, so their branches are uncovered:
//   - VariantStrategyString: the 3 enum arms (line 7-9);
//   - ParseVariantStrategy: the 3 match arms + the unknown→false arm (line 15-18);
//   - ValidateRagFusionConfig: each field's out-of-range fail() arm + the all-ok
//     return (line 30-46).
// All are pure, deterministic, fully reachable.
//
// Left uncovered ON PURPOSE (#4 — physically unreachable):
//   - rag_fusion_types.cpp:11 VariantStrategyString's post-switch `return
//     "paraphrase"` (the "unreachable for a valid enum" line): reaching it needs a
//     value cast outside the 3 enumerators (UB) — not a legitimate test input.
//
// onnx_complexity_backend.cpp is intentionally NOT supplemented here: the recon's
// line 53-54 "default vocab path" branch is ALREADY covered by
// test_onnx_complexity_backend.cpp's smoke test (it constructs
// OnnxComplexityBackend(ModelPath()) with the default empty vocab arg when the model
// is present), and its remaining uncovered arms (ONNX init/run exceptions) are
// ERROR-INJECTION-ONLY (need ORT to throw). No reachable net-new gap.
//
// Standalone NEW file; touches no existing test.
#include <gtest/gtest.h>

#include <string>

#include "cortrix/query/rag_fusion_types.h"

namespace cortrix::query {
namespace {

// ---------- VariantStrategyString: 3 enum arms ----------

TEST(RagFusionTypesBranchR7b, VariantStrategyStringAllThree) {
    EXPECT_STREQ(VariantStrategyString(VariantStrategy::kParaphrase), "paraphrase");
    EXPECT_STREQ(VariantStrategyString(VariantStrategy::kSubquery), "subquery");
    EXPECT_STREQ(VariantStrategyString(VariantStrategy::kReverse), "reverse");
}

// ---------- ParseVariantStrategy: 3 match arms + unknown ----------

TEST(RagFusionTypesBranchR7b, ParseVariantStrategyValidThree) {
    VariantStrategy out = VariantStrategy::kReverse;  // seed with a non-default
    ASSERT_TRUE(ParseVariantStrategy("paraphrase", &out));
    EXPECT_EQ(out, VariantStrategy::kParaphrase);
    ASSERT_TRUE(ParseVariantStrategy("subquery", &out));
    EXPECT_EQ(out, VariantStrategy::kSubquery);
    ASSERT_TRUE(ParseVariantStrategy("reverse", &out));
    EXPECT_EQ(out, VariantStrategy::kReverse);
}

// Unknown string → false, and `out` is left untouched (line 18 + the "out
// untouched" contract).
TEST(RagFusionTypesBranchR7b, ParseVariantStrategyUnknownReturnsFalse) {
    VariantStrategy out = VariantStrategy::kSubquery;  // sentinel
    EXPECT_FALSE(ParseVariantStrategy("invalid", &out));
    EXPECT_EQ(out, VariantStrategy::kSubquery) << "out must be left untouched on unknown";
}

// The null-out overload path: ParseVariantStrategy(..., nullptr) must still return
// the bool without dereferencing out (the `if (out)` guards).
TEST(RagFusionTypesBranchR7b, ParseVariantStrategyNullOut) {
    EXPECT_TRUE(ParseVariantStrategy("paraphrase", nullptr));
    EXPECT_FALSE(ParseVariantStrategy("nope", nullptr));
}

// ---------- ValidateRagFusionConfig: each fail() arm + the all-ok return ----------

// A default config is valid (the all-fields-in-range true return, line 46).
TEST(RagFusionTypesBranchR7b, ValidateDefaultConfigOk) {
    RagFusionConfig cfg;  // defaults: enabled=false, n=3, strategies=3, rrf_k=60, timeout=5000
    std::string field, range;
    EXPECT_TRUE(ValidateRagFusionConfig(cfg, &field, &range));
}

// variant_count below the min → the first fail() arm (line 30-31).
TEST(RagFusionTypesBranchR7b, ValidateVariantCountTooLow) {
    RagFusionConfig cfg;
    cfg.variant_count = 0;  // < kVariantCountMin (1)
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(cfg, &field, &range));
    EXPECT_EQ(field, "variant_count");
    EXPECT_EQ(range, "[1, 10]");
}

// variant_count above the max → same fail() arm, the upper-bound disjunct.
TEST(RagFusionTypesBranchR7b, ValidateVariantCountTooHigh) {
    RagFusionConfig cfg;
    cfg.variant_count = 11;  // > kVariantCountMax (10)
    std::string field;
    EXPECT_FALSE(ValidateRagFusionConfig(cfg, &field, nullptr));
    EXPECT_EQ(field, "variant_count");
}

// rrf_k <= 0 → the rrf_k fail() arm (line 34-35).
TEST(RagFusionTypesBranchR7b, ValidateRrfKNonPositive) {
    RagFusionConfig cfg;
    cfg.rrf_k = -1;
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(cfg, &field, &range));
    EXPECT_EQ(field, "rrf_k");
    EXPECT_EQ(range, "[1, ]");
}

// timeout_ms <= 0 → the timeout fail() arm (line 38-39). Keep the other fields
// valid so this is the first failure reached.
TEST(RagFusionTypesBranchR7b, ValidateTimeoutNonPositive) {
    RagFusionConfig cfg;
    cfg.timeout_ms = 0;
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(cfg, &field, &range));
    EXPECT_EQ(field, "timeout_ms");
    EXPECT_EQ(range, "[1, ]");
}

// locale outside zh|en → the locale fail() arm. This matters for benchmark/BEIR, where
// English benchmark queries must request the English prompt instead of zh.
TEST(RagFusionTypesBranchR7b, ValidateLocaleInvalid) {
    RagFusionConfig cfg;
    cfg.locale = "fr";
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(cfg, &field, &range));
    EXPECT_EQ(field, "locale");
    EXPECT_EQ(range, "zh|en");
}

// enabled=true with an EMPTY strategy list → the last fail() arm (line 43-44).
// Other fields valid so we reach this check.
TEST(RagFusionTypesBranchR7b, ValidateEnabledWithEmptyStrategies) {
    RagFusionConfig cfg;
    cfg.enabled = true;
    cfg.variant_strategies.clear();  // empty + enabled → invalid
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(cfg, &field, &range));
    EXPECT_EQ(field, "variant_strategies");
    EXPECT_EQ(range, "non-empty when enabled");
}

// enabled=FALSE with an empty strategy list → ALLOWED (the `cfg.enabled &&` short
// circuits, so the empty list is inert) → valid. The false side of the last guard.
TEST(RagFusionTypesBranchR7b, ValidateDisabledWithEmptyStrategiesOk) {
    RagFusionConfig cfg;
    cfg.enabled = false;
    cfg.variant_strategies.clear();  // empty but disabled → still valid
    EXPECT_TRUE(ValidateRagFusionConfig(cfg, nullptr, nullptr));
}

// ValidateRagFusionConfig with null out-params on a FAILING config → the fail()
// lambda's `if (field)` / `if (valid_range)` guards take their false arms (no
// deref) while still returning false.
TEST(RagFusionTypesBranchR7b, ValidateFailWithNullOutParams) {
    RagFusionConfig cfg;
    cfg.rrf_k = 0;  // invalid
    EXPECT_FALSE(ValidateRagFusionConfig(cfg));  // both out-params default nullptr
}

}  // namespace
}  // namespace cortrix::query
