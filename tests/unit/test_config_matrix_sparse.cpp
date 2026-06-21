#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/retrieval/sparse_ns_config.h"

// Exhaustive parameterized sweep over the F40 sparse top-K config resolver
// (NsSparseConfig::Parse / ResolveSparseTopK / ClampSparseTopK). Real bounds
// (50/100/200) and the IGlobalConfig key come from sparse_ns_config.h.
// Suite tokens are globally unique: SparseCfgParse* / SparseCfgClamp* /
// SparseCfgResolve*.
namespace cortrix::retrieval {
namespace {

// ===========================================================================
// Parse matrix: {valid override, absent, empty, "{}", null, wrong type,
//                unknown key, out-of-range stored, non-object error, bad JSON}
// ===========================================================================

struct SparseCfgParseCase {
    std::string blob;
    bool expect_ok;
    bool expect_has_value;
    int expect_value;  // only when expect_has_value
};

class SparseCfgParseMatrix : public ::testing::TestWithParam<SparseCfgParseCase> {};

TEST_P(SparseCfgParseMatrix, Parse) {
    const auto& c = GetParam();
    Result<NsSparseConfig> r = NsSparseConfig::Parse(c.blob);
    EXPECT_EQ(r.ok(), c.expect_ok) << "blob=" << c.blob;
    if (!c.expect_ok) {
        // Non-object / bad JSON must surface the canonical config-json error.
        EXPECT_NE(r.status().message().find("CX_ERR_INVALID_CONFIG_JSON"),
                  std::string::npos)
            << "blob=" << c.blob;
        return;
    }
    const NsSparseConfig& cfg = r.value();
    EXPECT_EQ(cfg.sparse_top_k.has_value(), c.expect_has_value) << "blob=" << c.blob;
    EXPECT_EQ(cfg.empty(), !c.expect_has_value) << "blob=" << c.blob;
    if (c.expect_has_value) {
        EXPECT_EQ(*cfg.sparse_top_k, c.expect_value) << "blob=" << c.blob;
    }
}

INSTANTIATE_TEST_SUITE_P(
    SparseCfg, SparseCfgParseMatrix,
    ::testing::Values(
        // --- override present (in range, at-bound, stored out-of-range) ---
        SparseCfgParseCase{R"({"sparse_top_k": 150})", true, true, 150},
        SparseCfgParseCase{R"({"sparse_top_k": 50})", true, true, 50},
        SparseCfgParseCase{R"({"sparse_top_k": 200})", true, true, 200},
        SparseCfgParseCase{R"({"sparse_top_k": 100})", true, true, 100},
        SparseCfgParseCase{R"({"sparse_top_k": 0})", true, true, 0},       // loads, clamps at resolve
        SparseCfgParseCase{R"({"sparse_top_k": 9999})", true, true, 9999}, // loads, clamps at resolve
        SparseCfgParseCase{R"({"sparse_top_k": -7})", true, true, -7},     // loads, clamps at resolve
        // --- absent / empty / inherit-global forms ---
        SparseCfgParseCase{"", true, false, 0},
        SparseCfgParseCase{"{}", true, false, 0},
        SparseCfgParseCase{"null", true, false, 0},
        SparseCfgParseCase{R"({"other": 1})", true, false, 0},  // unknown key only
        // --- wrong type for the key -> ignored (degrade to global) ---
        SparseCfgParseCase{R"({"sparse_top_k": "abc"})", true, false, 0},
        SparseCfgParseCase{R"({"sparse_top_k": true})", true, false, 0},
        SparseCfgParseCase{R"({"sparse_top_k": [1,2]})", true, false, 0},
        SparseCfgParseCase{R"({"sparse_top_k": {"x":1}})", true, false, 0},
        SparseCfgParseCase{R"({"sparse_top_k": null})", true, false, 0},
        // --- unknown keys ignored, valid key still parsed ---
        SparseCfgParseCase{R"({"future_key": 5, "sparse_top_k": 80})", true, true, 80},
        // --- valid JSON but not an object -> error ---
        SparseCfgParseCase{"[1,2,3]", false, false, 0},
        SparseCfgParseCase{"5", false, false, 0},
        SparseCfgParseCase{R"("a string")", false, false, 0},
        SparseCfgParseCase{"true", false, false, 0},
        // --- invalid JSON -> error ---
        SparseCfgParseCase{"{not json", false, false, 0},
        SparseCfgParseCase{"{\"sparse_top_k\":}", false, false, 0}));

// ===========================================================================
// Clamp matrix: window [kSparseTopKMin, kSparseTopKMax] = [50, 200].
// ===========================================================================

struct SparseCfgClampCase {
    int requested;
    int expect;
};

class SparseCfgClampMatrix : public ::testing::TestWithParam<SparseCfgClampCase> {};

TEST_P(SparseCfgClampMatrix, Clamp) {
    const auto& c = GetParam();
    EXPECT_EQ(ClampSparseTopK(c.requested), c.expect) << "requested=" << c.requested;
}

INSTANTIATE_TEST_SUITE_P(
    SparseCfg, SparseCfgClampMatrix,
    ::testing::Values(
        SparseCfgClampCase{kSparseTopKMin, kSparseTopKMin},       // 50
        SparseCfgClampCase{kSparseTopKMax, kSparseTopKMax},       // 200
        SparseCfgClampCase{kSparseTopKDefault, kSparseTopKDefault},
        SparseCfgClampCase{100, 100},
        SparseCfgClampCase{49, kSparseTopKMin},
        SparseCfgClampCase{0, kSparseTopKMin},
        SparseCfgClampCase{-1, kSparseTopKMin},
        SparseCfgClampCase{-9999, kSparseTopKMin},
        SparseCfgClampCase{201, kSparseTopKMax},
        SparseCfgClampCase{500, kSparseTopKMax},
        SparseCfgClampCase{9999, kSparseTopKMax}));

// ===========================================================================
// Resolve matrix: {NS override, NS clamp, global fallback, global clamp,
//                  no-global default, missing-key default}.
// ===========================================================================

enum class SparseGlobalKind { kNull, kEmpty, kSet };

struct SparseCfgResolveCase {
    bool ns_has_value;
    int ns_value;
    SparseGlobalKind global_kind;
    std::string global_value;  // only when kSet
    int expect;
};

class SparseCfgResolveMatrix : public ::testing::TestWithParam<SparseCfgResolveCase> {};

TEST_P(SparseCfgResolveMatrix, Resolve) {
    const auto& c = GetParam();
    NsSparseConfig ns;
    if (c.ns_has_value) ns.sparse_top_k = c.ns_value;

    std::unique_ptr<InMemoryGlobalConfig> g;
    const IGlobalConfig* gp = nullptr;
    if (c.global_kind != SparseGlobalKind::kNull) {
        g = std::make_unique<InMemoryGlobalConfig>();
        if (c.global_kind == SparseGlobalKind::kSet) {
            g->Set(kSparseTopKConfigKey, c.global_value);
        }
        gp = g.get();
    }
    EXPECT_EQ(ResolveSparseTopK(ns, gp), c.expect);
}

INSTANTIATE_TEST_SUITE_P(
    SparseCfg, SparseCfgResolveMatrix,
    ::testing::Values(
        // --- NS override present (wins, in range) ---
        SparseCfgResolveCase{true, 150, SparseGlobalKind::kSet, "75", 150},
        SparseCfgResolveCase{true, 120, SparseGlobalKind::kNull, "", 120},
        // --- NS override out-of-range -> clamp ---
        SparseCfgResolveCase{true, 9999, SparseGlobalKind::kNull, "", kSparseTopKMax},
        SparseCfgResolveCase{true, 1, SparseGlobalKind::kNull, "", kSparseTopKMin},
        SparseCfgResolveCase{true, 0, SparseGlobalKind::kSet, "175", kSparseTopKMin},
        SparseCfgResolveCase{true, 201, SparseGlobalKind::kSet, "60", kSparseTopKMax},
        // --- NS absent -> global default (in range) ---
        SparseCfgResolveCase{false, 0, SparseGlobalKind::kSet, "175", 175},
        SparseCfgResolveCase{false, 0, SparseGlobalKind::kSet, "50", 50},
        SparseCfgResolveCase{false, 0, SparseGlobalKind::kSet, "200", 200},
        // --- NS absent -> global out-of-range -> clamp ---
        SparseCfgResolveCase{false, 0, SparseGlobalKind::kSet, "300", kSparseTopKMax},
        SparseCfgResolveCase{false, 0, SparseGlobalKind::kSet, "10", kSparseTopKMin},
        // --- NS absent + global malformed value -> falls to compile default ---
        SparseCfgResolveCase{false, 0, SparseGlobalKind::kSet, "abc", kSparseTopKDefault},
        // --- NS absent + global missing key -> compile default ---
        SparseCfgResolveCase{false, 0, SparseGlobalKind::kEmpty, "", kSparseTopKDefault},
        // --- NS absent + null global -> compile default ---
        SparseCfgResolveCase{false, 0, SparseGlobalKind::kNull, "", kSparseTopKDefault}));

}  // namespace
}  // namespace cortrix::retrieval
