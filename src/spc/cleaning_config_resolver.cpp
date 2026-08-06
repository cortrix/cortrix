#include "cortrix/spc/cleaning_config_resolver.h"

#include "cortrix/catalog/config_resolver.h"  // ConfigResolver<CleaningConfig>
#include "cortrix/spc/cleaning_errors.h"       // CleaningStatus / kNsConfigMergeFailed

namespace cortrix::spc {

CleaningConfig LoadGlobalCleaningConfig(const IGlobalConfig* global) {
    CleaningConfig cfg;  // struct defaults (cleaning_types.h)
    if (global == nullptr) return cfg;

    // Each key OPTIONAL; a missing/typed-wrong key keeps the struct default
    // (defensive — a bad global value must never crash the SPC path).
    if (Result<bool> r = global->GetBool(kCleaningDedupEnabledKey); r.ok()) {
        cfg.dedup_enabled = r.value();
    }
    if (Result<float> r = global->GetFloat(kCleaningDedupThresholdKey); r.ok()) {
        cfg.dedup_similarity_threshold = static_cast<double>(r.value());
    }
    if (Result<bool> r = global->GetBool(kCleaningAnomalyDetectionEnabledKey);
        r.ok()) {
        cfg.anomaly_detection_enabled = r.value();
    }
    if (Result<int> r = global->GetInt(kCleaningMaxChunkCharsKey); r.ok()) {
        cfg.max_chunk_chars = r.value();
    }
    if (Result<int> r = global->GetInt(kCleaningPluginTimeoutMsKey); r.ok()) {
        cfg.plugin_timeout_ms = r.value();
    }
    return cfg;
}

Result<CleaningConfig> ResolveCleaningConfig(const CleaningConfig& global,
                                             const std::string& ns_blob) {
    // Layer global ← ns via the catalog template (shallow JSON-object merge). No
    // request layer in V1.0, so request = nullptr.
    catalog::ConfigResolver<CleaningConfig> resolver;
    Result<CleaningConfig> merged = resolver.Resolve(global, ns_blob);
    if (!merged.ok()) {
        // Re-badge the catalog merge failure as the cleaning-native error so the
        // boundary surfaces CX_ERR_CLEANING_NS_CONFIG_MERGE_FAILED (not a catalog code).
        return CleaningStatus(CleaningErrorCode::kNsConfigMergeFailed,
                              merged.status().message());
    }

    CleaningConfig eff = merged.value();
    // line 176: max_chunk_chars / plugin_timeout_ms are system-resource level
    // and NOT NS-overridable. The shallow merge would have honored a stray key for
    // them in the NS blob — re-assert the global values so only the 3 B-class
    // fields (dedup_enabled / dedup_similarity_threshold / anomaly_detection_enabled)
    // can be overridden per NS.
    eff.max_chunk_chars = global.max_chunk_chars;
    eff.plugin_timeout_ms = global.plugin_timeout_ms;
    return eff;
}

}  // namespace cortrix::spc
