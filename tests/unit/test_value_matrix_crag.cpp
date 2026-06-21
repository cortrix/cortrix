#include <gtest/gtest.h>

#include "cortrix/retrieval/crag_config.h"

// Value-matrix breadth coverage for the F37 CRAG threshold configuration
// invariant (CragConfig::IsValid) and the three-tier boundary semantics.
//
// Real behavior (crag_config.h, verified):
//   Defaults: enabled=true, threshold_correct=0.7, threshold_incorrect=0.3,
//             classifier_min_confidence=0.5, max_inference_retries=3.
//   IsValid() == (threshold_incorrect >= 0.0 && threshold_correct <= 1.0 &&
//                 threshold_incorrect <= threshold_correct).
//   Three-tier mapping (LabelFromScore, crag_evaluator.cpp):
//     score >= threshold_correct                          -> "correct"
//     score <  threshold_incorrect                        -> "incorrect"
//     otherwise (incorrect <= score < correct)            -> "ambiguous"
//   (LabelFromScore is private; the boundary semantics are documented + the
//    mapping is reproduced here as a pure reference predicate to exercise the
//    exact 0.7 / 0.3 / just-under / just-over boundaries deterministically.)
//
// All suite + fixture names globally unique (CragThresholdValMatrix* prefix).

namespace cortrix::retrieval {
namespace {

// Reference reproduction of the (private) LabelFromScore three-tier mapping,
// using the SAME comparisons as crag_evaluator.cpp LabelFromScore.
const char* RefLabel(const CragConfig& c, float score) {
    if (score >= c.threshold_correct) return "correct";
    if (score < c.threshold_incorrect) return "incorrect";
    return "ambiguous";
}

// ==========================================================================
// Default constants lock.
// ==========================================================================
TEST(CragThresholdValMatrixDefaults, LockedDefaults) {
    CragConfig c;
    EXPECT_TRUE(c.enabled);
    EXPECT_FLOAT_EQ(c.threshold_correct, 0.7f);
    EXPECT_FLOAT_EQ(c.threshold_incorrect, 0.3f);
    EXPECT_FLOAT_EQ(c.classifier_min_confidence, 0.5f);
    EXPECT_EQ(c.evaluation_method, "small_classifier");
    EXPECT_TRUE(c.fallback_to_correct_on_failure);
    EXPECT_TRUE(c.log_incorrect_to_obs);
    EXPECT_EQ(c.retrieval_fallback_strategy, "raw_results_only");
    EXPECT_EQ(c.max_inference_retries, 3);
    EXPECT_TRUE(c.IsValid());
}

// ==========================================================================
// IsValid matrix over (incorrect, correct) threshold pairs.
// ==========================================================================
struct CragThresholdValMatrixValidCase {
    float incorrect;
    float correct;
    bool valid;
};

class CragThresholdValMatrixValid
    : public ::testing::TestWithParam<CragThresholdValMatrixValidCase> {};

TEST_P(CragThresholdValMatrixValid, InvariantMatches) {
    const auto& tc = GetParam();
    CragConfig c;
    c.threshold_incorrect = tc.incorrect;
    c.threshold_correct = tc.correct;
    EXPECT_EQ(c.IsValid(), tc.valid);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, CragThresholdValMatrixValid,
    ::testing::Values(
        // valid: in range + ordered
        CragThresholdValMatrixValidCase{0.3f, 0.7f, true},   // defaults
        CragThresholdValMatrixValidCase{0.0f, 1.0f, true},   // full range
        CragThresholdValMatrixValidCase{0.0f, 0.0f, true},   // equal at 0
        CragThresholdValMatrixValidCase{1.0f, 1.0f, true},   // equal at 1
        CragThresholdValMatrixValidCase{0.5f, 0.5f, true},   // equal mid
        CragThresholdValMatrixValidCase{0.49f, 0.5f, true},  // just ordered
        // invalid: incorrect > correct (mis-ordered)
        CragThresholdValMatrixValidCase{0.7f, 0.3f, false},
        CragThresholdValMatrixValidCase{0.5001f, 0.5f, false},
        CragThresholdValMatrixValidCase{1.0f, 0.0f, false},
        // invalid: incorrect < 0
        CragThresholdValMatrixValidCase{-0.01f, 0.7f, false},
        CragThresholdValMatrixValidCase{-1.0f, 0.7f, false},
        // invalid: correct > 1
        CragThresholdValMatrixValidCase{0.3f, 1.01f, false},
        CragThresholdValMatrixValidCase{0.3f, 2.0f, false}));

// ==========================================================================
// Three-tier boundary mapping with the default 0.7 / 0.3 thresholds.
// Exact boundary, just-under, just-over.
// ==========================================================================
struct CragThresholdValMatrixLabelCase {
    float score;
    const char* label;
};

class CragThresholdValMatrixLabel
    : public ::testing::TestWithParam<CragThresholdValMatrixLabelCase> {};

TEST_P(CragThresholdValMatrixLabel, DefaultThresholdMapping) {
    const auto& tc = GetParam();
    CragConfig c;  // 0.3 / 0.7
    EXPECT_STREQ(RefLabel(c, tc.score), tc.label);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, CragThresholdValMatrixLabel,
    ::testing::Values(
        // correct tier: score >= 0.7
        CragThresholdValMatrixLabelCase{0.7f, "correct"},      // exact boundary
        CragThresholdValMatrixLabelCase{0.70001f, "correct"},  // just over
        CragThresholdValMatrixLabelCase{0.9f, "correct"},
        CragThresholdValMatrixLabelCase{1.0f, "correct"},
        CragThresholdValMatrixLabelCase{2.0f, "correct"},      // above range
        // ambiguous tier: 0.3 <= score < 0.7
        CragThresholdValMatrixLabelCase{0.69999f, "ambiguous"},  // just under 0.7
        CragThresholdValMatrixLabelCase{0.5f, "ambiguous"},
        CragThresholdValMatrixLabelCase{0.3f, "ambiguous"},      // exact lower bound
        CragThresholdValMatrixLabelCase{0.30001f, "ambiguous"},  // just over 0.3
        // incorrect tier: score < 0.3
        CragThresholdValMatrixLabelCase{0.29999f, "incorrect"},  // just under 0.3
        CragThresholdValMatrixLabelCase{0.1f, "incorrect"},
        CragThresholdValMatrixLabelCase{0.0f, "incorrect"},
        CragThresholdValMatrixLabelCase{-1.0f, "incorrect"}));   // below range

// ==========================================================================
// Mapping with custom thresholds (e.g. 0.2 / 0.8) — boundaries shift.
// ==========================================================================
TEST(CragThresholdValMatrixLabelCustom, CustomBoundaries) {
    CragConfig c;
    c.threshold_incorrect = 0.2f;
    c.threshold_correct = 0.8f;
    ASSERT_TRUE(c.IsValid());
    EXPECT_STREQ(RefLabel(c, 0.8f), "correct");
    EXPECT_STREQ(RefLabel(c, 0.79999f), "ambiguous");
    EXPECT_STREQ(RefLabel(c, 0.2f), "ambiguous");
    EXPECT_STREQ(RefLabel(c, 0.19999f), "incorrect");
}

// Degenerate: incorrect == correct collapses the ambiguous band.
TEST(CragThresholdValMatrixLabelCustom, CollapsedBandNoAmbiguous) {
    CragConfig c;
    c.threshold_incorrect = 0.5f;
    c.threshold_correct = 0.5f;
    ASSERT_TRUE(c.IsValid());
    EXPECT_STREQ(RefLabel(c, 0.5f), "correct");      // >= correct
    EXPECT_STREQ(RefLabel(c, 0.49999f), "incorrect");  // < incorrect
}

}  // namespace
}  // namespace cortrix::retrieval
