#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "cortrix/chunker/chunker_config.h"
#include "cortrix/chunker/chunker_guc.h"
#include "cortrix/common/status.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/retrieval/crag_config.h"

// Exhaustive per-field validation matrices over three config structs that expose
// an in-memory validity surface (no resolver/JSON needed):
//   CragConfig::IsValid()         — threshold ordering + [0,1] range invariant
//   ValidateRagFusionConfig()     — variant_count [1,10] / rrf_k>0 / timeout>0 /
//                                   non-empty strategies when enabled
//   ChunkerGuc::ValidateConfig()  — six ranged ints + child<=parent invariant
// Suite tokens globally unique: CragIsValidMatrix / RagFusionValidateMatrix /
// ChunkerValidateMatrix. Bounds use the ACTUAL header constants.

namespace {

// ===========================================================================
// CragConfig::IsValid — threshold invariant
//   0 <= threshold_incorrect <= threshold_correct <= 1
// ===========================================================================

struct CragIsValidCase {
    std::string name;
    float incorrect;
    float correct;
    bool expect_valid;
};

class CragIsValidMatrix : public ::testing::TestWithParam<CragIsValidCase> {};

TEST_P(CragIsValidMatrix, Threshold) {
    const auto& c = GetParam();
    cortrix::retrieval::CragConfig cfg;
    cfg.threshold_incorrect = c.incorrect;
    cfg.threshold_correct = c.correct;
    EXPECT_EQ(cfg.IsValid(), c.expect_valid) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Crag, CragIsValidMatrix,
    ::testing::Values(
        CragIsValidCase{"defaults_03_07", 0.3f, 0.7f, true},
        CragIsValidCase{"equal_thresholds_ok", 0.5f, 0.5f, true},
        CragIsValidCase{"both_zero_ok", 0.0f, 0.0f, true},
        CragIsValidCase{"both_one_ok", 1.0f, 1.0f, true},
        CragIsValidCase{"full_range_ok", 0.0f, 1.0f, true},
        CragIsValidCase{"incorrect_negative_bad", -0.01f, 0.7f, false},
        CragIsValidCase{"correct_over_one_bad", 0.3f, 1.01f, false},
        CragIsValidCase{"inverted_order_bad", 0.7f, 0.3f, false},
        CragIsValidCase{"incorrect_negative_correct_over_bad", -0.1f, 1.5f, false}),
    [](const ::testing::TestParamInfo<CragIsValidCase>& i) { return i.param.name; });

// ===========================================================================
// ValidateRagFusionConfig — field + valid_range out-params
// ===========================================================================

struct RagFusionCase {
    std::string name;
    std::function<void(cortrix::query::RagFusionConfig&)> mutate;
    bool expect_valid;
    std::string expect_field;  // checked only when !expect_valid
};

class RagFusionValidateMatrix
    : public ::testing::TestWithParam<RagFusionCase> {};

TEST_P(RagFusionValidateMatrix, Field) {
    const auto& c = GetParam();
    cortrix::query::RagFusionConfig cfg;  // defaults: enabled=false, vc=3, rrf_k=60, timeout=5000, locale=zh
    c.mutate(cfg);
    std::string field;
    std::string range;
    bool ok = cortrix::query::ValidateRagFusionConfig(cfg, &field, &range);
    EXPECT_EQ(ok, c.expect_valid) << c.name << " field=" << field;
    if (!c.expect_valid) {
        EXPECT_EQ(field, c.expect_field) << c.name;
        EXPECT_FALSE(range.empty()) << c.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Rag, RagFusionValidateMatrix,
    ::testing::Values(
        RagFusionCase{"defaults_valid", [](cortrix::query::RagFusionConfig&) {}, true, ""},
        // variant_count [1,10]
        RagFusionCase{"vc_min_1_ok", [](cortrix::query::RagFusionConfig& c) { c.variant_count = cortrix::query::kVariantCountMin; }, true, ""},
        RagFusionCase{"vc_max_10_ok", [](cortrix::query::RagFusionConfig& c) { c.variant_count = cortrix::query::kVariantCountMax; }, true, ""},
        RagFusionCase{"vc_zero_bad", [](cortrix::query::RagFusionConfig& c) { c.variant_count = 0; }, false, "variant_count"},
        RagFusionCase{"locale_en_ok", [](cortrix::query::RagFusionConfig& c) { c.locale = "en"; }, true, ""},
        RagFusionCase{"locale_bad", [](cortrix::query::RagFusionConfig& c) { c.locale = "fr"; }, false, "locale"},
        RagFusionCase{"vc_negative_bad", [](cortrix::query::RagFusionConfig& c) { c.variant_count = -1; }, false, "variant_count"},
        RagFusionCase{"vc_eleven_bad", [](cortrix::query::RagFusionConfig& c) { c.variant_count = 11; }, false, "variant_count"},
        // rrf_k > 0
        RagFusionCase{"rrf_k_one_ok", [](cortrix::query::RagFusionConfig& c) { c.rrf_k = 1; }, true, ""},
        RagFusionCase{"rrf_k_zero_bad", [](cortrix::query::RagFusionConfig& c) { c.rrf_k = 0; }, false, "rrf_k"},
        RagFusionCase{"rrf_k_neg_bad", [](cortrix::query::RagFusionConfig& c) { c.rrf_k = -60; }, false, "rrf_k"},
        // timeout_ms > 0
        RagFusionCase{"timeout_one_ok", [](cortrix::query::RagFusionConfig& c) { c.timeout_ms = 1; }, true, ""},
        RagFusionCase{"timeout_zero_bad", [](cortrix::query::RagFusionConfig& c) { c.timeout_ms = 0; }, false, "timeout_ms"},
        RagFusionCase{"timeout_neg_bad", [](cortrix::query::RagFusionConfig& c) { c.timeout_ms = -5; }, false, "timeout_ms"},
        // strategies non-empty when enabled (empty allowed when disabled)
        RagFusionCase{"empty_strategies_disabled_ok",
            [](cortrix::query::RagFusionConfig& c) { c.enabled = false; c.variant_strategies.clear(); }, true, ""},
        RagFusionCase{"empty_strategies_enabled_bad",
            [](cortrix::query::RagFusionConfig& c) { c.enabled = true; c.variant_strategies.clear(); }, false, "variant_strategies"},
        RagFusionCase{"enabled_with_strategies_ok",
            [](cortrix::query::RagFusionConfig& c) { c.enabled = true; }, true, ""}),
    [](const ::testing::TestParamInfo<RagFusionCase>& i) { return i.param.name; });

// ===========================================================================
// ChunkerGuc::ValidateConfig — six ranged ints + child<=parent invariant
// ===========================================================================

struct ChunkerValidateCase {
    std::string name;
    std::function<void(cortrix::chunker::ChunkerConfig&)> mutate;
    bool expect_ok;
};

class ChunkerValidateMatrix
    : public ::testing::TestWithParam<ChunkerValidateCase> {};

TEST_P(ChunkerValidateMatrix, Field) {
    const auto& c = GetParam();
    cortrix::chunker::ChunkerConfig cfg;  // locked defaults all valid
    c.mutate(cfg);
    cortrix::Status s = cortrix::chunker::ChunkerGuc::ValidateConfig(cfg);
    EXPECT_EQ(s.ok(), c.expect_ok) << c.name << " msg=" << s.message();
    if (!c.expect_ok) {
        EXPECT_EQ(s.code(), cortrix::StatusCode::kInvalidArgument) << c.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Chunker, ChunkerValidateMatrix,
    ::testing::Values(
        ChunkerValidateCase{"defaults_ok", [](cortrix::chunker::ChunkerConfig&) {}, true},
        // parent_size [256,8192]
        ChunkerValidateCase{"parent_min_256_ok", [](cortrix::chunker::ChunkerConfig& c) { c.parent_size = cortrix::chunker::guc::kParentSizeMin; }, true},
        ChunkerValidateCase{"parent_max_8192_ok", [](cortrix::chunker::ChunkerConfig& c) { c.parent_size = cortrix::chunker::guc::kParentSizeMax; }, true},
        ChunkerValidateCase{"parent_low_255_bad", [](cortrix::chunker::ChunkerConfig& c) { c.parent_size = 255; }, false},
        ChunkerValidateCase{"parent_high_8193_bad", [](cortrix::chunker::ChunkerConfig& c) { c.parent_size = 8193; }, false},
        // child_size [50,8192] and <= parent_size
        ChunkerValidateCase{"child_min_50_ok", [](cortrix::chunker::ChunkerConfig& c) { c.child_size = cortrix::chunker::guc::kChildSizeMin; }, true},
        ChunkerValidateCase{"child_low_49_bad", [](cortrix::chunker::ChunkerConfig& c) { c.child_size = 49; }, false},
        ChunkerValidateCase{"child_high_8193_bad", [](cortrix::chunker::ChunkerConfig& c) { c.child_size = 8193; }, false},
        ChunkerValidateCase{"child_bigger_than_parent_bad",
            [](cortrix::chunker::ChunkerConfig& c) { c.parent_size = 256; c.child_size = 512; }, false},
        ChunkerValidateCase{"child_equals_parent_ok",
            [](cortrix::chunker::ChunkerConfig& c) { c.parent_size = 512; c.child_size = 512; }, true},
        // child_overlap [0,100]
        ChunkerValidateCase{"overlap_zero_ok", [](cortrix::chunker::ChunkerConfig& c) { c.child_overlap = cortrix::chunker::guc::kChildOverlapMin; }, true},
        ChunkerValidateCase{"overlap_100_ok", [](cortrix::chunker::ChunkerConfig& c) { c.child_overlap = cortrix::chunker::guc::kChildOverlapMax; }, true},
        ChunkerValidateCase{"overlap_neg_bad", [](cortrix::chunker::ChunkerConfig& c) { c.child_overlap = -1; }, false},
        ChunkerValidateCase{"overlap_101_bad", [](cortrix::chunker::ChunkerConfig& c) { c.child_overlap = 101; }, false},
        // max_parents_per_doc [100,100000]
        ChunkerValidateCase{"maxparents_min_100_ok", [](cortrix::chunker::ChunkerConfig& c) { c.max_parents_per_doc = cortrix::chunker::guc::kMaxParentsMin; }, true},
        ChunkerValidateCase{"maxparents_max_ok", [](cortrix::chunker::ChunkerConfig& c) { c.max_parents_per_doc = cortrix::chunker::guc::kMaxParentsMax; }, true},
        ChunkerValidateCase{"maxparents_low_99_bad", [](cortrix::chunker::ChunkerConfig& c) { c.max_parents_per_doc = 99; }, false},
        ChunkerValidateCase{"maxparents_high_bad", [](cortrix::chunker::ChunkerConfig& c) { c.max_parents_per_doc = 100001; }, false},
        // fallback_to_flat_threshold shares the max_parents range
        ChunkerValidateCase{"fallback_low_bad", [](cortrix::chunker::ChunkerConfig& c) { c.fallback_to_flat_threshold = 99; }, false},
        ChunkerValidateCase{"fallback_high_bad", [](cortrix::chunker::ChunkerConfig& c) { c.fallback_to_flat_threshold = 100001; }, false},
        // children_per_parent_for_rerank [1,10]
        ChunkerValidateCase{"cpp_min_1_ok", [](cortrix::chunker::ChunkerConfig& c) { c.children_per_parent_for_rerank = cortrix::chunker::guc::kChildrenPerParentMin; }, true},
        ChunkerValidateCase{"cpp_max_10_ok", [](cortrix::chunker::ChunkerConfig& c) { c.children_per_parent_for_rerank = cortrix::chunker::guc::kChildrenPerParentMax; }, true},
        ChunkerValidateCase{"cpp_zero_bad", [](cortrix::chunker::ChunkerConfig& c) { c.children_per_parent_for_rerank = 0; }, false},
        ChunkerValidateCase{"cpp_11_bad", [](cortrix::chunker::ChunkerConfig& c) { c.children_per_parent_for_rerank = 11; }, false}),
    [](const ::testing::TestParamInfo<ChunkerValidateCase>& i) { return i.param.name; });

}  // namespace
