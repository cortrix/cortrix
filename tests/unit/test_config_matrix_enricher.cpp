#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "cortrix/spc_enricher.h"  // EnricherConfig
#include "cortrix/spc_enricher/enricher_config_resolver.h"

// Exhaustive parameterized sweep over the F03 enricher config resolution:
// EnricherNsConfig::Parse + the three-layer EnricherConfigResolver (request >
// NS > global for `enabled`; NS > global for model/threshold/template). Suite
// tokens globally unique: EnrichCfgParse* / EnrichCfgResolve* / EnrichCfgJson*.
namespace cortrix::spc {
namespace {

// ===========================================================================
// Parse matrix: 4 optional B-class fields (enabled / model / score_threshold /
// prompt_template_id) + wrong-type-ignored + non-object error + bad JSON.
// ===========================================================================

struct EnrichCfgParseCase {
    std::string blob;
    bool expect_ok;
    bool expect_empty;
    std::optional<bool> enabled;
    std::optional<std::string> model;
    std::optional<float> threshold;
    std::optional<std::string> tmpl;
};

class EnrichCfgParseMatrix : public ::testing::TestWithParam<EnrichCfgParseCase> {};

TEST_P(EnrichCfgParseMatrix, Parse) {
    const auto& c = GetParam();
    Result<EnricherNsConfig> r = EnricherNsConfig::Parse(c.blob);
    EXPECT_EQ(r.ok(), c.expect_ok) << "blob=" << c.blob;
    if (!c.expect_ok) {
        EXPECT_NE(r.status().message().find("CX_ERR_INVALID_CONFIG_JSON"),
                  std::string::npos)
            << "blob=" << c.blob;
        return;
    }
    const EnricherNsConfig& cfg = r.value();
    EXPECT_EQ(cfg.empty(), c.expect_empty) << "blob=" << c.blob;
    EXPECT_EQ(cfg.enabled, c.enabled) << "blob=" << c.blob;
    EXPECT_EQ(cfg.model, c.model) << "blob=" << c.blob;
    EXPECT_EQ(cfg.score_threshold.has_value(), c.threshold.has_value())
        << "blob=" << c.blob;
    if (c.threshold.has_value()) {
        EXPECT_FLOAT_EQ(*cfg.score_threshold, *c.threshold) << "blob=" << c.blob;
    }
    EXPECT_EQ(cfg.prompt_template_id, c.tmpl) << "blob=" << c.blob;
}

INSTANTIATE_TEST_SUITE_P(
    EnrichCfg, EnrichCfgParseMatrix,
    ::testing::Values(
        // full / partial overrides
        EnrichCfgParseCase{R"({"enabled": false, "model": "gpt-4o", "score_threshold": 0.5, "prompt_template_id": "p1"})",
                           true, false, false, std::string("gpt-4o"), 0.5f, std::string("p1")},
        EnrichCfgParseCase{R"({"enabled": true})", true, false, true, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{R"({"model": "claude"})", true, false, std::nullopt, std::string("claude"), std::nullopt, std::nullopt},
        EnrichCfgParseCase{R"({"score_threshold": 0.75})", true, false, std::nullopt, std::nullopt, 0.75f, std::nullopt},
        EnrichCfgParseCase{R"({"score_threshold": 2})", true, false, std::nullopt, std::nullopt, 2.0f, std::nullopt},  // int is a number
        EnrichCfgParseCase{R"({"prompt_template_id": "zh"})", true, false, std::nullopt, std::nullopt, std::nullopt, std::string("zh")},
        // absent / empty / inherit-global
        EnrichCfgParseCase{"", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{"{}", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{"null", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{R"({"unknown": 1})", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        // wrong type per field -> ignored
        EnrichCfgParseCase{R"({"enabled": 1})", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{R"({"model": 5})", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{R"({"score_threshold": "high"})", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{R"({"prompt_template_id": true})", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{R"({"model": null})", true, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        // unknown ignored, valid still parsed
        EnrichCfgParseCase{R"({"x": 9, "enabled": false, "model": "m"})",
                           true, false, false, std::string("m"), std::nullopt, std::nullopt},
        // valid JSON not an object -> error
        EnrichCfgParseCase{"[1]", false, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{"3", false, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        EnrichCfgParseCase{R"("s")", false, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        // invalid JSON -> error
        EnrichCfgParseCase{"{bad", false, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt}));

// ===========================================================================
// Resolve matrix (parsed NS path): enabled = request > NS > global;
// model/threshold/template = NS > global. Global EnricherConfig defaults:
// enabled=true, model="gpt-4o-mini", score_threshold=0.0, template="default-zh".
// ===========================================================================

struct EnrichCfgResolveCase {
    bool global_enabled;
    std::optional<bool> ns_enabled;
    std::optional<std::string> ns_model;
    std::optional<float> ns_threshold;
    std::optional<std::string> ns_tmpl;
    std::optional<bool> req_enrich;
    bool expect_enabled;
    std::string expect_model;
    float expect_threshold;
    std::string expect_tmpl;
};

class EnrichCfgResolveMatrix : public ::testing::TestWithParam<EnrichCfgResolveCase> {};

TEST_P(EnrichCfgResolveMatrix, Resolve) {
    const auto& c = GetParam();
    EnricherConfig global;  // defaults from spc_enricher.h
    global.enabled = c.global_enabled;
    EnricherConfigResolver resolver(global);

    EnricherNsConfig ns;
    ns.enabled = c.ns_enabled;
    ns.model = c.ns_model;
    ns.score_threshold = c.ns_threshold;
    ns.prompt_template_id = c.ns_tmpl;

    EnricherRequestParams req;
    req.enrich = c.req_enrich;

    EnricherConfig eff = resolver.Resolve(ns, req);
    EXPECT_EQ(eff.enabled, c.expect_enabled);
    EXPECT_EQ(eff.model, c.expect_model);
    EXPECT_FLOAT_EQ(eff.score_threshold, c.expect_threshold);
    EXPECT_EQ(eff.prompt_template_id, c.expect_tmpl);
}

INSTANTIATE_TEST_SUITE_P(
    EnrichCfg, EnrichCfgResolveMatrix,
    ::testing::Values(
        // all absent -> global defaults
        EnrichCfgResolveCase{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                             true, "gpt-4o-mini", 0.0f, "default-zh"},
        EnrichCfgResolveCase{false, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                             false, "gpt-4o-mini", 0.0f, "default-zh"},
        // NS enabled overrides global
        EnrichCfgResolveCase{true, false, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                             false, "gpt-4o-mini", 0.0f, "default-zh"},
        EnrichCfgResolveCase{false, true, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                             true, "gpt-4o-mini", 0.0f, "default-zh"},
        // request overrides NS and global for enabled (the  section 2.9 example)
        EnrichCfgResolveCase{true, false, std::nullopt, std::nullopt, std::nullopt, true,
                             true, "gpt-4o-mini", 0.0f, "default-zh"},
        EnrichCfgResolveCase{false, true, std::nullopt, std::nullopt, std::nullopt, false,
                             false, "gpt-4o-mini", 0.0f, "default-zh"},
        // NS model / threshold / template override global (no request layer)
        EnrichCfgResolveCase{true, std::nullopt, std::string("claude"), std::nullopt, std::nullopt, std::nullopt,
                             true, "claude", 0.0f, "default-zh"},
        EnrichCfgResolveCase{true, std::nullopt, std::nullopt, 0.6f, std::nullopt, std::nullopt,
                             true, "gpt-4o-mini", 0.6f, "default-zh"},
        EnrichCfgResolveCase{true, std::nullopt, std::nullopt, std::nullopt, std::string("en"), std::nullopt,
                             true, "gpt-4o-mini", 0.0f, "en"},
        // combined NS overrides + request enable
        EnrichCfgResolveCase{false, false, std::string("m2"), 0.9f, std::string("t2"), true,
                             true, "m2", 0.9f, "t2"}));

// ===========================================================================
// Resolve-from-JSON error propagation matrix.
// ===========================================================================

struct EnrichCfgJsonCase {
    std::string blob;
    bool expect_ok;
};

class EnrichCfgJsonMatrix : public ::testing::TestWithParam<EnrichCfgJsonCase> {};

TEST_P(EnrichCfgJsonMatrix, ResolveFromJson) {
    const auto& c = GetParam();
    EnricherConfig global;
    EnricherConfigResolver resolver(global);
    Result<EnricherConfig> r = resolver.Resolve(c.blob);
    EXPECT_EQ(r.ok(), c.expect_ok) << "blob=" << c.blob;
}

INSTANTIATE_TEST_SUITE_P(
    EnrichCfg, EnrichCfgJsonMatrix,
    ::testing::Values(
        EnrichCfgJsonCase{"", true},
        EnrichCfgJsonCase{"{}", true},
        EnrichCfgJsonCase{"null", true},
        EnrichCfgJsonCase{R"({"enabled": false})", true},
        EnrichCfgJsonCase{R"({"model": "x"})", true},
        EnrichCfgJsonCase{R"({"score_threshold": "bad"})", true},  // ignored, not error
        EnrichCfgJsonCase{"[1]", false},
        EnrichCfgJsonCase{"42", false},
        EnrichCfgJsonCase{"{bad", false}));

}  // namespace
}  // namespace cortrix::spc
