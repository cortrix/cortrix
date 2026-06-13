#pragma once
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/i_global_config.h"
#include "cortrix/common/result.h"

namespace cortrix::retrieval {

/// F40 top-K config bounds (F40-6: K=100 default, NS-configurable 50-200).
inline constexpr int kSparseTopKDefault = 100;
inline constexpr int kSparseTopKMin = 50;
inline constexpr int kSparseTopKMax = 200;

/// The IGlobalConfig key holding the process-wide default sparse top-K. Read via
/// the generic IGlobalConfig::GetInt accessor (the canonical config surface does
/// not have an F40-typed getter — that would be an F40 D3 reverse hook to
/// IGlobalConfig at D3.5; standalone reads the generic key).
inline constexpr const char* kSparseTopKConfigKey = "retrieval.sparse_top_k";

/// Per-namespace F40 sparse overrides parsed from a `*_config` JSONB blob (mirrors
/// the F02 NsRerankerConfig pattern, topic 2.2/2.4). V1 = the single
/// NS-overridable B-class field `sparse_top_k`; every field is OPTIONAL so an
/// absent key inherits the global default at resolve time. A-class caps
/// (posting_list_cap / cache size) are process-global, NOT here.
struct NsSparseConfig {
    std::optional<int> sparse_top_k;  ///< NS top-K override (clamped to 50-200 at resolve)

    bool empty() const { return !sparse_top_k.has_value(); }

    /// Parse a `*_config` JSONB blob. "" / "{}" / SQL NULL → all-unset (inherit
    /// global). Unknown keys ignored; a key present with the wrong JSON type is
    /// ignored (defensive — a malformed NS value must never crash the query
    /// path, it degrades to the global default). A blob that is valid JSON but
    /// not an object → CX_ERR_INVALID_CONFIG_JSON (InvalidArgument); invalid JSON
    /// → InvalidArgument. An in-range-but out-of-bounds sparse_top_k is parsed
    /// as-is here (range enforcement is at Resolve, so a stored 0/9999 still
    /// loads, then clamps).
    static Result<NsSparseConfig> Parse(const std::string& json_blob);
};

/// Resolve the effective top-K for a query: NS override (if set) else the global
/// default (IGlobalConfig key, else kSparseTopKDefault), clamped to [50, 200]
/// (F40-6). `global` may be null (tests / no-config) → falls back to the
/// compile-time default before clamping.
int ResolveSparseTopK(const NsSparseConfig& ns_cfg, const IGlobalConfig* global);

/// Clamp an arbitrary requested top-K to the F40-6 [50, 200] window. Exposed for
/// tests + call sites that already have a raw value.
int ClampSparseTopK(int requested);

}  // namespace cortrix::retrieval
