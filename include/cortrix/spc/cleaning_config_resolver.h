#pragma once
#include <string>

#include "cortrix/common/i_global_config.h"
#include "cortrix/common/result.h"
#include "cortrix/spc/cleaning_types.h"

namespace cortrix::spc {

/// The IGlobalConfig keys holding the process-wide cleaning defaults.
/// Read via the generic IGlobalConfig accessors — the canonical config surface has
/// no cleaning-typed getter (that would be a reverse hook into IGlobalConfig);
/// standalone reads the generic keys. Absent keys fall back to the CleaningConfig
/// struct defaults (cleaning_types.h §3.1).
inline constexpr const char* kCleaningDedupEnabledKey =
    "cleaning.dedup_enabled";
inline constexpr const char* kCleaningDedupThresholdKey =
    "cleaning.dedup_similarity_threshold";
inline constexpr const char* kCleaningAnomalyDetectionEnabledKey =
    "cleaning.anomaly_detection_enabled";
inline constexpr const char* kCleaningMaxChunkCharsKey =
    "cleaning.max_chunk_chars";
inline constexpr const char* kCleaningPluginTimeoutMsKey =
    "cleaning.plugin_timeout_ms";

/// Read the process-wide CleaningConfig defaults from IGlobalConfig (the §3.4
/// "global" layer). A null `global` (tests / no-config) returns the struct
/// defaults; a missing individual key falls back to that field's struct default.
/// This is the lowest of the three layers (global → NS → request).
CleaningConfig LoadGlobalCleaningConfig(const IGlobalConfig* global);

/// Resolve the effective CleaningConfig for one namespace = global ← NS override
/// (three-layer order global → ns.cleaning_config → request; V1.0 has no
/// request layer). Reuses the ConfigResolver<CleaningConfig> template (
/// mandate) for the JSON-object shallow merge, then RE-ASSERTS the two
/// resource-level fields (max_chunk_chars / plugin_timeout_ms) back to `global`:
/// those are system-resource level and NOT NS-overridable (§3.4 line 176), so a
/// stray key for them in the NS blob is ignored.
///
/// `ns_blob` is the raw `namespaces.cleaning_config` JSONB ("" / "{}" / SQL NULL =
/// inherit global entirely). Returns CX_ERR_F10_NS_CONFIG_MERGE_FAILED (§5.1) when
/// the blob is present but not a JSON object, or when the merged result fails to
/// deserialize — so the SPC path can surface the Agent-friendly merge error rather
/// than silently using a wrong config.
Result<CleaningConfig> ResolveCleaningConfig(const CleaningConfig& global,
                                             const std::string& ns_blob);

}  // namespace cortrix::spc
