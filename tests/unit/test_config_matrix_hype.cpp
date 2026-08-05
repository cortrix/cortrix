#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/hype_config.h"
#include "cortrix/spc/hype_ns_config.h"

// Exhaustive parameterized sweep over the HyPE K config resolver
// (NsHyPEConfig::Parse / ResolveHypeK / ClampHypeK). Real bounds (1/3/10) +
// the IGlobalConfig key come from hype_config.h. Suite tokens globally unique:
// HypeCfgParse* / HypeCfgClamp* / HypeCfgResolve*.
namespace cortrix::spc {
namespace {

// ===========================================================================
// Parse matrix.
// ===========================================================================

struct HypeCfgParseCase {
    std::string blob;
    bool expect_ok;
    bool expect_has_value;
    int expect_value;
};

class HypeCfgParseMatrix : public ::testing::TestWithParam<HypeCfgParseCase> {};

TEST_P(HypeCfgParseMatrix, Parse) {
    const auto& c = GetParam();
    Result<NsHyPEConfig> r = NsHyPEConfig::Parse(c.blob);
    EXPECT_EQ(r.ok(), c.expect_ok) << "blob=" << c.blob;
    if (!c.expect_ok) {
        EXPECT_NE(r.status().message().find("CX_ERR_INVALID_CONFIG_JSON"),
                  std::string::npos)
            << "blob=" << c.blob;
        return;
    }
    const NsHyPEConfig& cfg = r.value();
    EXPECT_EQ(cfg.questions_per_chunk.has_value(), c.expect_has_value)
        << "blob=" << c.blob;
    EXPECT_EQ(cfg.empty(), !c.expect_has_value) << "blob=" << c.blob;
    if (c.expect_has_value) {
        EXPECT_EQ(*cfg.questions_per_chunk, c.expect_value) << "blob=" << c.blob;
    }
}

INSTANTIATE_TEST_SUITE_P(
    HypeCfg, HypeCfgParseMatrix,
    ::testing::Values(
        // override present (in range / at-bound / stored out-of-range)
        HypeCfgParseCase{R"({"questions_per_chunk": 5})", true, true, 5},
        HypeCfgParseCase{R"({"questions_per_chunk": 1})", true, true, 1},
        HypeCfgParseCase{R"({"questions_per_chunk": 10})", true, true, 10},
        HypeCfgParseCase{R"({"questions_per_chunk": 3})", true, true, 3},
        HypeCfgParseCase{R"({"questions_per_chunk": 0})", true, true, 0},
        HypeCfgParseCase{R"({"questions_per_chunk": 99})", true, true, 99},
        HypeCfgParseCase{R"({"questions_per_chunk": -2})", true, true, -2},
        // absent / empty / inherit-global
        HypeCfgParseCase{"", true, false, 0},
        HypeCfgParseCase{"{}", true, false, 0},
        HypeCfgParseCase{"null", true, false, 0},
        HypeCfgParseCase{R"({"unrelated": 7})", true, false, 0},
        // wrong type -> ignored
        HypeCfgParseCase{R"({"questions_per_chunk": "x"})", true, false, 0},
        HypeCfgParseCase{R"({"questions_per_chunk": true})", true, false, 0},
        HypeCfgParseCase{R"({"questions_per_chunk": [1]})", true, false, 0},
        HypeCfgParseCase{R"({"questions_per_chunk": null})", true, false, 0},
        // unknown ignored, valid still parsed
        HypeCfgParseCase{R"({"x": 1, "questions_per_chunk": 4})", true, true, 4},
        // valid JSON not an object -> error
        HypeCfgParseCase{"[1,2]", false, false, 0},
        HypeCfgParseCase{"7", false, false, 0},
        HypeCfgParseCase{R"("str")", false, false, 0},
        HypeCfgParseCase{"false", false, false, 0},
        // invalid JSON -> error
        HypeCfgParseCase{"{bad", false, false, 0}));

// ===========================================================================
// Clamp matrix: window [kHypeQuestionsMin, kHypeQuestionsMax] = [1, 10].
// ===========================================================================

struct HypeCfgClampCase {
    int requested;
    int expect;
};

class HypeCfgClampMatrix : public ::testing::TestWithParam<HypeCfgClampCase> {};

TEST_P(HypeCfgClampMatrix, Clamp) {
    const auto& c = GetParam();
    EXPECT_EQ(ClampHypeK(c.requested), c.expect) << "requested=" << c.requested;
}

INSTANTIATE_TEST_SUITE_P(
    HypeCfg, HypeCfgClampMatrix,
    ::testing::Values(
        HypeCfgClampCase{kHypeQuestionsMin, kHypeQuestionsMin},  // 1
        HypeCfgClampCase{kHypeQuestionsMax, kHypeQuestionsMax},  // 10
        HypeCfgClampCase{kHypeQuestionsDefault, kHypeQuestionsDefault},
        HypeCfgClampCase{5, 5},
        HypeCfgClampCase{0, kHypeQuestionsMin},
        HypeCfgClampCase{-3, kHypeQuestionsMin},
        HypeCfgClampCase{11, kHypeQuestionsMax},
        HypeCfgClampCase{100, kHypeQuestionsMax}));

// ===========================================================================
// Resolve matrix.
// ===========================================================================

enum class HypeGlobalKind { kNull, kEmpty, kSet };

struct HypeCfgResolveCase {
    bool ns_has_value;
    int ns_value;
    HypeGlobalKind global_kind;
    std::string global_value;
    int expect;
};

class HypeCfgResolveMatrix : public ::testing::TestWithParam<HypeCfgResolveCase> {};

TEST_P(HypeCfgResolveMatrix, Resolve) {
    const auto& c = GetParam();
    NsHyPEConfig ns;
    if (c.ns_has_value) ns.questions_per_chunk = c.ns_value;

    std::unique_ptr<InMemoryGlobalConfig> g;
    const IGlobalConfig* gp = nullptr;
    if (c.global_kind != HypeGlobalKind::kNull) {
        g = std::make_unique<InMemoryGlobalConfig>();
        if (c.global_kind == HypeGlobalKind::kSet) {
            g->Set(kHypeQuestionsPerChunkKey, c.global_value);
        }
        gp = g.get();
    }
    EXPECT_EQ(ResolveHypeK(ns, gp), c.expect);
}

INSTANTIATE_TEST_SUITE_P(
    HypeCfg, HypeCfgResolveMatrix,
    ::testing::Values(
        // NS override present (in range)
        HypeCfgResolveCase{true, 5, HypeGlobalKind::kSet, "2", 5},
        HypeCfgResolveCase{true, 7, HypeGlobalKind::kNull, "", 7},
        // NS override out-of-range -> clamp
        HypeCfgResolveCase{true, 99, HypeGlobalKind::kNull, "", kHypeQuestionsMax},
        HypeCfgResolveCase{true, 0, HypeGlobalKind::kNull, "", kHypeQuestionsMin},
        HypeCfgResolveCase{true, -4, HypeGlobalKind::kSet, "3", kHypeQuestionsMin},
        HypeCfgResolveCase{true, 11, HypeGlobalKind::kSet, "8", kHypeQuestionsMax},
        // NS absent -> global default (in range)
        HypeCfgResolveCase{false, 0, HypeGlobalKind::kSet, "7", 7},
        HypeCfgResolveCase{false, 0, HypeGlobalKind::kSet, "1", 1},
        HypeCfgResolveCase{false, 0, HypeGlobalKind::kSet, "10", 10},
        // NS absent -> global out-of-range -> clamp
        HypeCfgResolveCase{false, 0, HypeGlobalKind::kSet, "50", kHypeQuestionsMax},
        HypeCfgResolveCase{false, 0, HypeGlobalKind::kSet, "0", kHypeQuestionsMin},
        // NS absent + global malformed -> compile default
        HypeCfgResolveCase{false, 0, HypeGlobalKind::kSet, "abc", kHypeQuestionsDefault},
        // NS absent + missing key -> compile default
        HypeCfgResolveCase{false, 0, HypeGlobalKind::kEmpty, "", kHypeQuestionsDefault},
        // NS absent + null global -> compile default
        HypeCfgResolveCase{false, 0, HypeGlobalKind::kNull, "", kHypeQuestionsDefault}));

}  // namespace
}  // namespace cortrix::spc
