#include <gtest/gtest.h>

#include <string>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/cleaning_config_resolver.h"
#include "cortrix/spc/cleaning_types.h"

// F10 §3.4 NS-level cleaning_config three-layer resolve (S5): global ← NS override
// (V1.0 has no request layer). The 3 B-class fields (dedup_enabled /
// dedup_similarity_threshold / anomaly_detection_enabled) are NS-overridable; the 2
// resource-level fields (max_chunk_chars / plugin_timeout_ms) are global-only.
namespace cortrix::spc {
namespace {

// A non-default global so "inherit global" is distinguishable from struct defaults.
CleaningConfig GlobalCfg() {
    CleaningConfig g;
    g.dedup_enabled = true;
    g.dedup_similarity_threshold = 0.90;
    g.anomaly_detection_enabled = true;
    g.max_chunk_chars = 8000;
    g.plugin_timeout_ms = 2500;
    return g;
}

// ---------- ResolveCleaningConfig: NS override (the 3 B-class fields) ----------

// (1) Empty / "{}" / null blob → inherit global entirely (no override).
TEST(CleaningConfigResolveTest, EmptyBlob_InheritsGlobal) {
    const CleaningConfig g = GlobalCfg();
    for (const std::string blob : {std::string(""), std::string("{}"),
                                   std::string("null")}) {
        Result<CleaningConfig> r = ResolveCleaningConfig(g, blob);
        ASSERT_TRUE(r.ok()) << "blob=" << blob << " : " << r.status().message();
        EXPECT_EQ(r.value().dedup_enabled, g.dedup_enabled);
        EXPECT_DOUBLE_EQ(r.value().dedup_similarity_threshold,
                         g.dedup_similarity_threshold);
        EXPECT_EQ(r.value().anomaly_detection_enabled, g.anomaly_detection_enabled);
        EXPECT_EQ(r.value().max_chunk_chars, g.max_chunk_chars);
        EXPECT_EQ(r.value().plugin_timeout_ms, g.plugin_timeout_ms);
    }
}

// (2) Partial override: only dedup_similarity_threshold set in the NS blob; the
// other two B-class fields keep the global value.
TEST(CleaningConfigResolveTest, PartialOverride_ThresholdOnly) {
    const CleaningConfig g = GlobalCfg();
    Result<CleaningConfig> r =
        ResolveCleaningConfig(g, R"({"dedup_similarity_threshold": 0.80})");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_DOUBLE_EQ(r.value().dedup_similarity_threshold, 0.80);  // overridden
    EXPECT_EQ(r.value().dedup_enabled, g.dedup_enabled);           // inherited
    EXPECT_EQ(r.value().anomaly_detection_enabled,
              g.anomaly_detection_enabled);                        // inherited
}

// (3) Full override: all 3 B-class fields set in the NS blob.
TEST(CleaningConfigResolveTest, FullOverride_AllThreeBClassFields) {
    const CleaningConfig g = GlobalCfg();
    Result<CleaningConfig> r = ResolveCleaningConfig(
        g,
        R"({"dedup_enabled": false, "dedup_similarity_threshold": 0.70,
            "anomaly_detection_enabled": false})");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(r.value().dedup_enabled);
    EXPECT_DOUBLE_EQ(r.value().dedup_similarity_threshold, 0.70);
    EXPECT_FALSE(r.value().anomaly_detection_enabled);
}

// (4) Missing-field fallback: an NS blob that sets only anomaly_detection_enabled
// leaves dedup_* at the global default.
TEST(CleaningConfigResolveTest, MissingFields_FallBackToGlobal) {
    const CleaningConfig g = GlobalCfg();
    Result<CleaningConfig> r =
        ResolveCleaningConfig(g, R"({"anomaly_detection_enabled": false})");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(r.value().anomaly_detection_enabled);             // overridden
    EXPECT_EQ(r.value().dedup_enabled, g.dedup_enabled);          // global default
    EXPECT_DOUBLE_EQ(r.value().dedup_similarity_threshold,
                     g.dedup_similarity_threshold);                // global default
}

// (5) Resource-level fields are NOT NS-overridable (§3.4 line 176): a stray
// max_chunk_chars / plugin_timeout_ms in the NS blob is ignored — the global value
// is re-asserted.
TEST(CleaningConfigResolveTest, ResourceLevelFields_NotNsOverridable) {
    const CleaningConfig g = GlobalCfg();
    Result<CleaningConfig> r = ResolveCleaningConfig(
        g, R"({"max_chunk_chars": 99, "plugin_timeout_ms": 1,
               "dedup_enabled": false})");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().max_chunk_chars, g.max_chunk_chars);     // ignored → global
    EXPECT_EQ(r.value().plugin_timeout_ms, g.plugin_timeout_ms);  // ignored → global
    EXPECT_FALSE(r.value().dedup_enabled);  // a real B-class override still applies
}

// (6) A blob that is valid JSON but not an object → CX_ERR_F10_NS_CONFIG_MERGE_FAILED.
TEST(CleaningConfigResolveTest, NonObjectBlob_MergeFailed) {
    const CleaningConfig g = GlobalCfg();
    Result<CleaningConfig> r = ResolveCleaningConfig(g, "[1,2,3]");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F10_NS_CONFIG_MERGE_FAILED"),
              std::string::npos)
        << r.status().message();
}

// (7) Invalid JSON → merge failed (the SPC path surfaces the F10 error, never a
// silently-wrong config).
TEST(CleaningConfigResolveTest, InvalidJsonBlob_MergeFailed) {
    const CleaningConfig g = GlobalCfg();
    Result<CleaningConfig> r = ResolveCleaningConfig(g, "{not json");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F10_NS_CONFIG_MERGE_FAILED"),
              std::string::npos)
        << r.status().message();
}

// ---------- LoadGlobalCleaningConfig: the global (lowest) layer ----------

// (8) Null IGlobalConfig → struct defaults (cleaning_types.h §3.1).
TEST(CleaningConfigGlobalTest, NullConfig_StructDefaults) {
    CleaningConfig c = LoadGlobalCleaningConfig(nullptr);
    CleaningConfig def;  // struct defaults
    EXPECT_EQ(c.dedup_enabled, def.dedup_enabled);
    EXPECT_DOUBLE_EQ(c.dedup_similarity_threshold, def.dedup_similarity_threshold);
    EXPECT_EQ(c.anomaly_detection_enabled, def.anomaly_detection_enabled);
    EXPECT_EQ(c.max_chunk_chars, def.max_chunk_chars);
    EXPECT_EQ(c.plugin_timeout_ms, def.plugin_timeout_ms);
}

// (9) Keys present in IGlobalConfig are read; absent keys fall back to the struct
// default (per-field, defensive).
TEST(CleaningConfigGlobalTest, ReadsPresentKeys_FallsBackOnAbsent) {
    InMemoryGlobalConfig gc;
    gc.Set(kCleaningDedupThresholdKey, "0.5");
    gc.Set(kCleaningAnomalyDetectionEnabledKey, "false");
    gc.Set(kCleaningMaxChunkCharsKey, "12345");
    // kCleaningDedupEnabledKey / kCleaningPluginTimeoutMsKey deliberately unset.

    CleaningConfig c = LoadGlobalCleaningConfig(&gc);
    CleaningConfig def;
    EXPECT_DOUBLE_EQ(c.dedup_similarity_threshold, 0.5);  // from config
    EXPECT_FALSE(c.anomaly_detection_enabled);            // from config
    EXPECT_EQ(c.max_chunk_chars, 12345);                  // from config
    EXPECT_EQ(c.dedup_enabled, def.dedup_enabled);        // absent → struct default
    EXPECT_EQ(c.plugin_timeout_ms, def.plugin_timeout_ms);  // absent → struct default
}

// (10) End-to-end: global from IGlobalConfig, then NS override on top — the two
// layers compose (this is exactly how bootstrap wires it).
TEST(CleaningConfigGlobalTest, GlobalThenNsOverride_Composes) {
    InMemoryGlobalConfig gc;
    gc.Set(kCleaningDedupThresholdKey, "0.92");
    gc.Set(kCleaningMaxChunkCharsKey, "7000");
    const CleaningConfig global = LoadGlobalCleaningConfig(&gc);

    Result<CleaningConfig> r =
        ResolveCleaningConfig(global, R"({"dedup_similarity_threshold": 0.60})");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_DOUBLE_EQ(r.value().dedup_similarity_threshold, 0.60);  // NS wins
    EXPECT_EQ(r.value().max_chunk_chars, 7000);  // resource-level: from global
}

}  // namespace
}  // namespace cortrix::spc
