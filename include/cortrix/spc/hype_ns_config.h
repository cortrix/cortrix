#pragma once
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/i_global_config.h"
#include "cortrix/common/result.h"
#include "cortrix/spc/hype_config.h"  // kHypeQuestions* bounds/keys

namespace cortrix::spc {

/// Per-namespace HyPE overrides parsed from a `*_config` JSONB blob (mirrors
/// the NsRerankerConfig / NsSparseConfig pattern). V1 = the single
/// NS-overridable B-class field `questions_per_chunk` (default 3,
/// NS-configurable 1-10); every field is OPTIONAL so an absent key inherits the
/// global default at resolve time. prompt_version is A-class (global) in Phase 1
/// (NS-switchable prompt = Phase 2, §14), so it is NOT here.
struct NsHyPEConfig {
    std::optional<int> questions_per_chunk;  ///< NS K override (clamped 1-10 at resolve)

    bool empty() const { return !questions_per_chunk.has_value(); }

    /// Parse a `*_config` JSONB blob. "" / "{}" / SQL NULL → all-unset (inherit
    /// global). Unknown keys ignored; a key with the wrong JSON type is ignored
    /// (defensive — a malformed NS value must never crash the index path, it
    /// degrades to the global default). A blob that is valid JSON but not an
    /// object → CX_ERR_INVALID_CONFIG_JSON (InvalidArgument). Range enforcement is
    /// at Resolve (a stored out-of-bounds value still loads, then clamps).
    static Result<NsHyPEConfig> Parse(const std::string& json_blob);
};

/// Resolve the effective K (questions per chunk): NS override (if set) else the
/// global default (IGlobalConfig key kHypeQuestionsPerChunkKey, else
/// kHypeQuestionsDefault), clamped to [1, 10]. `global` may be null
/// (tests / no-config) → falls back to the compile-time default before clamping.
int ResolveHypeK(const NsHyPEConfig& ns_cfg, const IGlobalConfig* global);

/// Clamp an arbitrary requested K to the [1, 10] window. Exposed for tests
/// + call sites with a raw value.
int ClampHypeK(int requested);

}  // namespace cortrix::spc
