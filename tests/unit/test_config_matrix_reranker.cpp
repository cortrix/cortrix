#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "cortrix/reranker/reranker_config.h"
#include "cortrix/reranker/reranker_config_resolver.h"
#include "cortrix/reranker/reranker_ns_config.h"

// Exhaustive parameterized sweep over the F02 reranker config resolution:
// NsRerankerConfig::Parse + the three-layer RerankerConfigResolver (request >
// NS > global) + EffectiveRerankerConfig::ComputeTopN. Suite tokens globally
// unique: RerankCfgParse* / RerankCfgResolve* / RerankCfgTopN*.
namespace cortrix::reranker {
namespace {

// ===========================================================================
// Parse matrix: 3 optional B-class fields (enabled / candidate_multiplier /
// max_candidates) + wrong-type-ignored + non-object error + bad JSON.
// ===========================================================================

struct RerankCfgParseCase {
    std::string blob;
    bool expect_ok;
    bool expect_empty;
    std::optional<bool> enabled;
    std::optional<int> mult;
    std::optional<int> maxc;
};

class RerankCfgParseMatrix : public ::testing::TestWithParam<RerankCfgParseCase> {};

TEST_P(RerankCfgParseMatrix, Parse) {
    const auto& c = GetParam();
    Result<NsRerankerConfig> r = NsRerankerConfig::Parse(c.blob);
    EXPECT_EQ(r.ok(), c.expect_ok) << "blob=" << c.blob;
    if (!c.expect_ok) {
        EXPECT_NE(r.status().message().find("CX_ERR_INVALID_CONFIG_JSON"),
                  std::string::npos)
            << "blob=" << c.blob;
        return;
    }
    const NsRerankerConfig& cfg = r.value();
    EXPECT_EQ(cfg.empty(), c.expect_empty) << "blob=" << c.blob;
    EXPECT_EQ(cfg.enabled, c.enabled) << "blob=" << c.blob;
    EXPECT_EQ(cfg.candidate_multiplier, c.mult) << "blob=" << c.blob;
    EXPECT_EQ(cfg.max_candidates, c.maxc) << "blob=" << c.blob;
}

INSTANTIATE_TEST_SUITE_P(
    RerankCfg, RerankCfgParseMatrix,
    ::testing::Values(
        // full / partial overrides
        RerankCfgParseCase{R"({"enabled": false, "candidate_multiplier": 5, "max_candidates": 80})",
                           true, false, false, 5, 80},
        RerankCfgParseCase{R"({"enabled": true})", true, false, true, std::nullopt, std::nullopt},
        RerankCfgParseCase{R"({"candidate_multiplier": 2})", true, false, std::nullopt, 2, std::nullopt},
        RerankCfgParseCase{R"({"max_candidates": 25})", true, false, std::nullopt, std::nullopt, 25},
        // absent / empty / inherit-global
        RerankCfgParseCase{"", true, true, std::nullopt, std::nullopt, std::nullopt},
        RerankCfgParseCase{"{}", true, true, std::nullopt, std::nullopt, std::nullopt},
        RerankCfgParseCase{"null", true, true, std::nullopt, std::nullopt, std::nullopt},
        RerankCfgParseCase{R"({"unknown": 9})", true, true, std::nullopt, std::nullopt, std::nullopt},
        // wrong type per field -> ignored (degrade to global)
        RerankCfgParseCase{R"({"enabled": "yes"})", true, true, std::nullopt, std::nullopt, std::nullopt},
        RerankCfgParseCase{R"({"candidate_multiplier": "3"})", true, true, std::nullopt, std::nullopt, std::nullopt},
        RerankCfgParseCase{R"({"max_candidates": true})", true, true, std::nullopt, std::nullopt, std::nullopt},
        RerankCfgParseCase{R"({"enabled": null})", true, true, std::nullopt, std::nullopt, std::nullopt},
        // unknown ignored, valid fields still parsed
        RerankCfgParseCase{R"({"z": 1, "enabled": true, "max_candidates": 40})",
                           true, false, true, std::nullopt, 40},
        // valid JSON not an object -> error
        RerankCfgParseCase{"[1,2]", false, true, std::nullopt, std::nullopt, std::nullopt},
        RerankCfgParseCase{"5", false, true, std::nullopt, std::nullopt, std::nullopt},
        RerankCfgParseCase{R"("s")", false, true, std::nullopt, std::nullopt, std::nullopt},
        // invalid JSON -> error
        RerankCfgParseCase{"{bad json", false, true, std::nullopt, std::nullopt, std::nullopt}));

// ===========================================================================
// Resolve matrix (parsed NS path): enabled = request > NS > global;
// mult/max = NS > global. Global defaults: enabled via ctor flag,
// candidate_multiplier=3, max_candidates=50 (RerankerConfig defaults).
// ===========================================================================

struct RerankCfgResolveCase {
    bool global_enabled;
    std::optional<bool> ns_enabled;
    std::optional<int> ns_mult;
    std::optional<int> ns_maxc;
    std::optional<bool> req_rerank;
    bool expect_enabled;
    int expect_mult;
    int expect_maxc;
};

class RerankCfgResolveMatrix : public ::testing::TestWithParam<RerankCfgResolveCase> {};

TEST_P(RerankCfgResolveMatrix, Resolve) {
    const auto& c = GetParam();
    RerankerConfig global;  // candidate_multiplier=3, max_candidates=50 (defaults)
    RerankerConfigResolver resolver(global, c.global_enabled);

    NsRerankerConfig ns;
    ns.enabled = c.ns_enabled;
    ns.candidate_multiplier = c.ns_mult;
    ns.max_candidates = c.ns_maxc;

    RerankerRequestParams req;
    req.rerank = c.req_rerank;

    EffectiveRerankerConfig eff = resolver.Resolve(ns, req);
    EXPECT_EQ(eff.enabled, c.expect_enabled);
    EXPECT_EQ(eff.candidate_multiplier, c.expect_mult);
    EXPECT_EQ(eff.max_candidates, c.expect_maxc);
}

INSTANTIATE_TEST_SUITE_P(
    RerankCfg, RerankCfgResolveMatrix,
    ::testing::Values(
        // all absent -> global defaults
        RerankCfgResolveCase{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt, true, 3, 50},
        RerankCfgResolveCase{false, std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, 3, 50},
        // NS enabled overrides global
        RerankCfgResolveCase{true, false, std::nullopt, std::nullopt, std::nullopt, false, 3, 50},
        RerankCfgResolveCase{false, true, std::nullopt, std::nullopt, std::nullopt, true, 3, 50},
        // request overrides both NS and global (the  section 2.6 example)
        RerankCfgResolveCase{true, false, std::nullopt, std::nullopt, true, true, 3, 50},
        RerankCfgResolveCase{false, true, std::nullopt, std::nullopt, false, false, 3, 50},
        RerankCfgResolveCase{true, std::nullopt, std::nullopt, std::nullopt, false, false, 3, 50},
        // NS mult / max override global (no request layer for these)
        RerankCfgResolveCase{true, std::nullopt, 5, std::nullopt, std::nullopt, true, 5, 50},
        RerankCfgResolveCase{true, std::nullopt, std::nullopt, 80, std::nullopt, true, 3, 80},
        RerankCfgResolveCase{true, std::nullopt, 7, 120, std::nullopt, true, 7, 120},
        // combined: request on, NS sets caps
        RerankCfgResolveCase{false, false, 4, 30, true, true, 4, 30}));

// ===========================================================================
// Resolve-from-JSON error propagation matrix.
// ===========================================================================

struct RerankCfgResolveJsonCase {
    std::string blob;
    bool expect_ok;
};

class RerankCfgResolveJsonMatrix
    : public ::testing::TestWithParam<RerankCfgResolveJsonCase> {};

TEST_P(RerankCfgResolveJsonMatrix, ResolveFromJson) {
    const auto& c = GetParam();
    RerankerConfig global;
    RerankerConfigResolver resolver(global, /*global_enabled=*/true);
    Result<EffectiveRerankerConfig> r = resolver.Resolve(c.blob);
    EXPECT_EQ(r.ok(), c.expect_ok) << "blob=" << c.blob;
}

INSTANTIATE_TEST_SUITE_P(
    RerankCfg, RerankCfgResolveJsonMatrix,
    ::testing::Values(
        RerankCfgResolveJsonCase{"", true},
        RerankCfgResolveJsonCase{"{}", true},
        RerankCfgResolveJsonCase{"null", true},
        RerankCfgResolveJsonCase{R"({"enabled": false})", true},
        RerankCfgResolveJsonCase{R"({"enabled": "bad"})", true},  // ignored, not an error
        RerankCfgResolveJsonCase{"[1]", false},
        RerankCfgResolveJsonCase{"42", false},
        RerankCfgResolveJsonCase{"{bad", false}));

// ===========================================================================
// ComputeTopN matrix: top_N = min(top_k * mult, max_candidates), floored at
// top_k; top_k <= 0 -> 0; mult floored at 1.
// ===========================================================================

struct RerankCfgTopNCase {
    int mult;
    int maxc;
    int top_k;
    int expect;
};

class RerankCfgTopNMatrix : public ::testing::TestWithParam<RerankCfgTopNCase> {};

TEST_P(RerankCfgTopNMatrix, ComputeTopN) {
    const auto& c = GetParam();
    EffectiveRerankerConfig eff;
    eff.enabled = true;
    eff.candidate_multiplier = c.mult;
    eff.max_candidates = c.maxc;
    EXPECT_EQ(eff.ComputeTopN(c.top_k), c.expect)
        << "mult=" << c.mult << " maxc=" << c.maxc << " top_k=" << c.top_k;
}

INSTANTIATE_TEST_SUITE_P(
    RerankCfg, RerankCfgTopNMatrix,
    ::testing::Values(
        RerankCfgTopNCase{3, 50, 10, 30},        // 30 < 50 -> 30
        RerankCfgTopNCase{3, 50, 20, 50},        // 60 capped at 50, still >= top_k
        RerankCfgTopNCase{3, 50, 50, 50},        // cap == top_k floor
        RerankCfgTopNCase{3, 10, 20, 20},        // cap 10 < top_k 20 -> floor to 20
        RerankCfgTopNCase{1, 50, 10, 10},        // mult 1 -> top_k
        RerankCfgTopNCase{0, 50, 10, 10},        // mult floored to 1 -> top_k
        RerankCfgTopNCase{5, 1000, 4, 20},       // 20 < cap
        RerankCfgTopNCase{3, 50, 0, 0},          // top_k 0 -> 0
        RerankCfgTopNCase{3, 50, -5, 0}));       // top_k negative -> 0

}  // namespace
}  // namespace cortrix::reranker
