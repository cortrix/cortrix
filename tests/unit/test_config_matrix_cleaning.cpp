#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cortrix/catalog/config_resolver.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/cleaning_config_resolver.h"
#include "cortrix/spc/cleaning_types.h"

// Exhaustive parameterized sweep over the cleaning config resolution
// (LoadGlobalCleaningConfig from IGlobalConfig + ResolveCleaningConfig NS merge)
// and the generic catalog::ConfigResolver<CleaningConfig> it builds on.
// Suite tokens globally unique: CleanCfgGlobal* / CleanCfgResolve* /
// CleanCfgGeneric*.
namespace cortrix::spc {
namespace {

// CleaningConfig struct defaults (cleaning_types.h  section 3.1).
constexpr double kDefDedupThreshold = 0.95;
constexpr int kDefMaxChunkChars = 10000;
constexpr int kDefPluginTimeoutMs = 3000;

// ===========================================================================
// LoadGlobalCleaningConfig matrix: each key OPTIONAL; missing/typed-wrong key
// keeps the struct default. null global -> all defaults.
// ===========================================================================

enum class CleanGlobalKind { kNull, kEmpty, kSet };

struct CleanCfgGlobalCase {
    CleanGlobalKind kind;
    std::string dedup_enabled;       // for kSet, "" = key not present
    std::string threshold;
    std::string anomaly_enabled;
    std::string max_chunk_chars;
    std::string plugin_timeout_ms;
    bool expect_dedup_enabled;
    double expect_threshold;
    bool expect_anomaly_enabled;
    int expect_max_chunk_chars;
    int expect_plugin_timeout_ms;
};

class CleanCfgGlobalMatrix : public ::testing::TestWithParam<CleanCfgGlobalCase> {};

TEST_P(CleanCfgGlobalMatrix, LoadGlobal) {
    const auto& c = GetParam();
    std::unique_ptr<InMemoryGlobalConfig> g;
    const IGlobalConfig* gp = nullptr;
    if (c.kind != CleanGlobalKind::kNull) {
        g = std::make_unique<InMemoryGlobalConfig>();
        if (c.kind == CleanGlobalKind::kSet) {
            if (!c.dedup_enabled.empty())
                g->Set(kCleaningDedupEnabledKey, c.dedup_enabled);
            if (!c.threshold.empty())
                g->Set(kCleaningDedupThresholdKey, c.threshold);
            if (!c.anomaly_enabled.empty())
                g->Set(kCleaningAnomalyDetectionEnabledKey, c.anomaly_enabled);
            if (!c.max_chunk_chars.empty())
                g->Set(kCleaningMaxChunkCharsKey, c.max_chunk_chars);
            if (!c.plugin_timeout_ms.empty())
                g->Set(kCleaningPluginTimeoutMsKey, c.plugin_timeout_ms);
        }
        gp = g.get();
    }
    CleaningConfig cfg = LoadGlobalCleaningConfig(gp);
    EXPECT_EQ(cfg.dedup_enabled, c.expect_dedup_enabled);
    // dedup_similarity_threshold is a float; compare with float tolerance so a
    // double literal like 0.8 (0.80000000000000004) does not mismatch 0.8f.
    EXPECT_FLOAT_EQ(cfg.dedup_similarity_threshold, c.expect_threshold);
    EXPECT_EQ(cfg.anomaly_detection_enabled, c.expect_anomaly_enabled);
    EXPECT_EQ(cfg.max_chunk_chars, c.expect_max_chunk_chars);
    EXPECT_EQ(cfg.plugin_timeout_ms, c.expect_plugin_timeout_ms);
}

INSTANTIATE_TEST_SUITE_P(
    CleanCfg, CleanCfgGlobalMatrix,
    ::testing::Values(
        // null global -> all struct defaults
        CleanCfgGlobalCase{CleanGlobalKind::kNull, "", "", "", "", "",
                           true, kDefDedupThreshold, true, kDefMaxChunkChars, kDefPluginTimeoutMs},
        // empty config (no keys) -> all struct defaults
        CleanCfgGlobalCase{CleanGlobalKind::kEmpty, "", "", "", "", "",
                           true, kDefDedupThreshold, true, kDefMaxChunkChars, kDefPluginTimeoutMs},
        // each key set to a non-default value
        CleanCfgGlobalCase{CleanGlobalKind::kSet, "false", "0.8", "false", "5000", "1000",
                           false, 0.8, false, 5000, 1000},
        // partial: only threshold overridden, rest defaults
        CleanCfgGlobalCase{CleanGlobalKind::kSet, "", "0.5", "", "", "",
                           true, 0.5, true, kDefMaxChunkChars, kDefPluginTimeoutMs},
        // partial: only resource fields overridden
        CleanCfgGlobalCase{CleanGlobalKind::kSet, "", "", "", "20000", "8000",
                           true, kDefDedupThreshold, true, 20000, 8000},
        // wrong-type values -> keep struct default (defensive)
        CleanCfgGlobalCase{CleanGlobalKind::kSet, "notabool", "abc", "xx", "yy", "zz",
                           true, kDefDedupThreshold, true, kDefMaxChunkChars, kDefPluginTimeoutMs},
        // bool "1"/"0" forms accepted by GetBool
        CleanCfgGlobalCase{CleanGlobalKind::kSet, "0", "", "1", "", "",
                           false, kDefDedupThreshold, true, kDefMaxChunkChars, kDefPluginTimeoutMs}));

// ===========================================================================
// ResolveCleaningConfig matrix: global <- NS merge, then resource-level fields
// (max_chunk_chars / plugin_timeout_ms) RE-ASSERTED to global (not NS-overridable).
// Errors on non-object NS blob -> CX_ERR_CLEANING_NS_CONFIG_MERGE_FAILED.
// ===========================================================================

struct CleanCfgResolveCase {
    std::string ns_blob;
    bool expect_ok;
    bool expect_dedup_enabled;
    double expect_threshold;
    bool expect_anomaly_enabled;
    // resource fields always == global (200000 / 9999) regardless of NS blob
};

class CleanCfgResolveMatrix : public ::testing::TestWithParam<CleanCfgResolveCase> {};

TEST_P(CleanCfgResolveMatrix, Resolve) {
    const auto& c = GetParam();
    CleaningConfig global;
    global.dedup_enabled = true;
    global.dedup_similarity_threshold = 0.95;
    global.anomaly_detection_enabled = true;
    global.max_chunk_chars = 200000;   // distinctive resource-level values
    global.plugin_timeout_ms = 9999;

    Result<CleaningConfig> r = ResolveCleaningConfig(global, c.ns_blob);
    EXPECT_EQ(r.ok(), c.expect_ok) << "blob=" << c.ns_blob;
    if (!c.expect_ok) {
        EXPECT_NE(r.status().message().find("CX_ERR_CLEANING_NS_CONFIG_MERGE_FAILED"),
                  std::string::npos)
            << "blob=" << c.ns_blob;
        return;
    }
    const CleaningConfig& eff = r.value();
    EXPECT_EQ(eff.dedup_enabled, c.expect_dedup_enabled) << "blob=" << c.ns_blob;
    EXPECT_DOUBLE_EQ(eff.dedup_similarity_threshold, c.expect_threshold)
        << "blob=" << c.ns_blob;
    EXPECT_EQ(eff.anomaly_detection_enabled, c.expect_anomaly_enabled)
        << "blob=" << c.ns_blob;
    // Resource-level fields must always be re-asserted to global.
    EXPECT_EQ(eff.max_chunk_chars, 200000) << "blob=" << c.ns_blob;
    EXPECT_EQ(eff.plugin_timeout_ms, 9999) << "blob=" << c.ns_blob;
}

INSTANTIATE_TEST_SUITE_P(
    CleanCfg, CleanCfgResolveMatrix,
    ::testing::Values(
        // absent / empty / null -> inherit global
        CleanCfgResolveCase{"", true, true, 0.95, true},
        CleanCfgResolveCase{"{}", true, true, 0.95, true},
        CleanCfgResolveCase{"null", true, true, 0.95, true},
        // NS overrides the 3 B-class fields
        CleanCfgResolveCase{R"({"dedup_enabled": false})", true, false, 0.95, true},
        CleanCfgResolveCase{R"({"dedup_similarity_threshold": 0.7})", true, true, 0.7, true},
        CleanCfgResolveCase{R"({"anomaly_detection_enabled": false})", true, true, 0.95, false},
        CleanCfgResolveCase{R"({"dedup_enabled": false, "dedup_similarity_threshold": 0.6, "anomaly_detection_enabled": false})",
                           true, false, 0.6, false},
        // NS tries to override resource-level fields -> IGNORED (re-asserted to global)
        CleanCfgResolveCase{R"({"max_chunk_chars": 1, "plugin_timeout_ms": 1})", true, true, 0.95, true},
        CleanCfgResolveCase{R"({"dedup_enabled": false, "max_chunk_chars": 42})", true, false, 0.95, true},
        // unknown keys merge in harmlessly (still deserializes)
        CleanCfgResolveCase{R"({"future_field": 1, "dedup_enabled": false})", true, false, 0.95, true},
        // non-object blob -> merge error
        CleanCfgResolveCase{"[1,2,3]", false, true, 0.95, true},
        CleanCfgResolveCase{"5", false, true, 0.95, true},
        CleanCfgResolveCase{R"("str")", false, true, 0.95, true},
        // invalid JSON -> merge error
        CleanCfgResolveCase{"{bad", false, true, 0.95, true}));

// ===========================================================================
// Generic catalog::ConfigResolver<CleaningConfig> matrix (the catalog template the
// Cleaning resolver builds on). Covers the three-layer merge with a request layer +
// the strict request whitelist, which the cleaning wrapper does not exercise.
// ===========================================================================

struct CleanCfgGenericCase {
    std::string ns_json;
    bool has_request;
    bool req_dedup_enabled;          // request layer CleaningConfig.dedup_enabled
    std::string allowed_field;       // "" = no whitelist; else the single allowed key
    bool expect_ok;
    bool expect_dedup_enabled;
};

class CleanCfgGenericMatrix : public ::testing::TestWithParam<CleanCfgGenericCase> {};

TEST_P(CleanCfgGenericMatrix, GenericResolve) {
    const auto& c = GetParam();
    CleaningConfig global;  // dedup_enabled defaults to true
    global.dedup_enabled = true;

    catalog::ConfigResolver<CleaningConfig> resolver;
    if (!c.allowed_field.empty()) {
        resolver.SetRequestAllowedFields({c.allowed_field});
    }

    CleaningConfig req;
    req.dedup_enabled = c.req_dedup_enabled;
    const CleaningConfig* reqp = c.has_request ? &req : nullptr;

    Result<CleaningConfig> r = resolver.Resolve(global, c.ns_json, reqp);
    EXPECT_EQ(r.ok(), c.expect_ok) << "ns=" << c.ns_json;
    if (!c.expect_ok) {
        EXPECT_NE(r.status().message().find("CX_ERR_INVALID_CONFIG_JSON"),
                  std::string::npos)
            << "ns=" << c.ns_json;
        return;
    }
    EXPECT_EQ(r.value().dedup_enabled, c.expect_dedup_enabled) << "ns=" << c.ns_json;
}

INSTANTIATE_TEST_SUITE_P(
    CleanCfg, CleanCfgGenericMatrix,
    ::testing::Values(
        // no NS, no request -> global (true)
        CleanCfgGenericCase{"", false, false, "", true, true},
        CleanCfgGenericCase{"{}", false, false, "", true, true},
        CleanCfgGenericCase{"null", false, false, "", true, true},
        // NS overrides global
        CleanCfgGenericCase{R"({"dedup_enabled": false})", false, false, "", true, false},
        // request present but NO whitelist -> request ignored (global wins)
        CleanCfgGenericCase{"", true, false, "", true, true},
        // request whitelisted on the field -> request wins over global
        CleanCfgGenericCase{"", true, false, "dedup_enabled", true, false},
        // request whitelisted on a DIFFERENT field -> request ignored
        CleanCfgGenericCase{"", true, false, "anomaly_detection_enabled", true, true},
        // request overrides NS when whitelisted (highest layer)
        CleanCfgGenericCase{R"({"dedup_enabled": true})", true, false, "dedup_enabled", true, false},
        // non-object NS -> error
        CleanCfgGenericCase{"[1]", false, false, "", false, true},
        CleanCfgGenericCase{"42", false, false, "", false, true},
        // invalid JSON -> error
        CleanCfgGenericCase{"{bad", false, false, "", false, true}));

}  // namespace
}  // namespace cortrix::spc
