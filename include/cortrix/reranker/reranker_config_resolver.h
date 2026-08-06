#pragma once
#include <optional>
#include <string>

#include "cortrix/common/result.h"
#include "cortrix/reranker/reranker_config.h"
#include "cortrix/reranker/reranker_ns_config.h"

namespace cortrix::reranker {

/// The reranker request-level parameter set. V1 exposes
/// exactly ONE request-level control: `rerank` boolean. candidate_multiplier /
/// max_candidates are NS-level business params, never per-request (topic 2.5), so
/// they are intentionally absent here. Unset (`std::nullopt`) = the request does
/// not override `enabled` (defaults to TRUE at the SQL signature, but a caller
/// that omits it leaves the NS/global decision intact).
struct RerankerRequestParams {
    std::optional<bool> rerank;  ///< pgcortrix_search(..., rerank BOOLEAN DEFAULT TRUE)
};

/// The fully-resolved reranker decision for one query.
struct EffectiveRerankerConfig {
    bool enabled;               ///< whether to run the reranker at all
    int  candidate_multiplier;  ///< effective top_N multiplier
    int  max_candidates;        ///< effective top_N cap

    /// top_N = min(top_k × candidate_multiplier, max_candidates) (E2), floored
    /// at top_k so reranking never returns fewer than the requested results when
    /// enough candidates exist. top_k ≤ 0 → 0.
    int ComputeTopN(int top_k) const;
};

/// RerankerConfigResolver — the three-layer priority resolver:
///
///     request (rerank=true/false)   ── highest, only sets `enabled`
///         ↓ overrides
///     NS (namespaces.reranker_config JSONB)   ── enabled / mult / max_candidates
///         ↓ overrides
///     global default (reranker.* GUC)         ── lowest
///
/// `enabled` cascades request → NS → global; candidate_multiplier / max_candidates
/// cascade NS → global only (no request layer, topic 2.5). This implements the
/// example: NS `{"enabled":false}` + request `rerank=true` → enabled.
///
/// Built from the global RerankerConfig (the resident defaults) plus a global
/// `reranker.enabled` GUC (which is NOT a field of RerankerConfig — it is a
/// separate NS-overridable GUC). Standalone: this resolves whatever it is
/// handed. Phasing of the live wiring that hands it those inputs:
///   - the request `rerank` arg from the SQL function is the Phase-1 per-request
///     override (example); wiring it in is integration cross-Feature work.
///   - reading the NS blob from the INSRouter cache (namespaces.reranker_config) is
///     PHASE-2 (per-NS model / score_threshold land "after the
///     LlamaCppReranker is introduced"). The resolver is the Phase-2-ready pre-build
///     — NOT a integration wiring gap.
class RerankerConfigResolver {
public:
    /// @param global          resident defaults (candidate_multiplier / max_candidates)
    /// @param global_enabled  the global `reranker.enabled` GUC (default true)
    RerankerConfigResolver(const RerankerConfig& global, bool global_enabled = true)
        : global_(global), global_enabled_(global_enabled) {}

    /// Resolve global ← ns ← request. `ns_json` is the raw NS `reranker_config`
    /// blob ("" / "{}" = inherit global). Returns CX_ERR (InvalidArgument) only
    /// when ns_json is present but not a JSON object (a malformed individual value
    /// is ignored → global fallback, never an error — the query path must not
    /// fail on bad NS metadata).
    Result<EffectiveRerankerConfig> Resolve(
        const std::string& ns_json,
        const RerankerRequestParams& request = {}) const;

    /// Same, but with an already-parsed NS config (skips JSON parsing).
    EffectiveRerankerConfig Resolve(
        const NsRerankerConfig& ns,
        const RerankerRequestParams& request = {}) const;

    bool global_enabled() const { return global_enabled_; }

private:
    RerankerConfig global_;
    bool global_enabled_;
};

}  // namespace cortrix::reranker
