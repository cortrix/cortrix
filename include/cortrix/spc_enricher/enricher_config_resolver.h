#pragma once
#include <optional>
#include <string>

#include "cortrix/common/result.h"
#include "cortrix/spc_enricher.h"  // EnricherConfig

namespace cortrix::spc {

/// Per-namespace enricher overrides parsed from `namespaces.enricher_config`
/// JSONB (design §2.8 / topic 2.2). V1 = the 4 B-class fields; every field is
/// OPTIONAL so an absent key falls back to the global default at resolve time
/// (topic 2.4 "a missing key falls back to the global default"). Unknown keys ignored (forward-compatible).
///
/// A-class resident GUCs (endpoint / api_key / workers / queue / timeouts /
/// circuit breaker / budget / probe) are deliberately NOT here — process-global,
/// not NS-overridable (topic 2.1).
struct EnricherNsConfig {
    std::optional<bool>        enabled;            ///< NS on/off for enrichment
    std::optional<std::string> model;              ///< NS model override
    std::optional<float>       score_threshold;    ///< NS score threshold
    std::optional<std::string> prompt_template_id; ///< NS prompt template

    /// True iff no field is set ('{}' blob = inherit global entirely).
    bool empty() const {
        return !enabled && !model && !score_threshold && !prompt_template_id;
    }

    /// Parse an `enricher_config` JSONB blob. "" or "{}" → all-unset (inherit
    /// global). Unknown keys ignored; a key present with the wrong JSON type is
    /// ignored too (defensive — malformed NS metadata must never crash the SPC
    /// path, it degrades to the global default). A blob that is valid JSON but
    /// not an object (e.g. "[]" / "5") → CX_ERR (InvalidArgument); not-valid-JSON
    /// → InvalidArgument.
    static Result<EnricherNsConfig> Parse(const std::string& json_blob);
};

/// Request-level enricher params (design §2.5 / topic 2.5). V1 exposes exactly ONE
/// request control: `enrich` boolean. model / score_threshold / prompt_template_id
/// are NS-level, never per-request (topic 2.5), so absent here. Unset = the request
/// does not override `enabled`.
struct EnricherRequestParams {
    std::optional<bool> enrich;  ///< pgcortrix request enrich=true/false
};

/// EnricherConfigResolver — the §2.9 three-layer priority resolver:
///
///     request (enrich=true/false)         ── highest, only sets `enabled`
///         ↓ overrides
///     NS (namespaces.enricher_config JSONB) ── enabled / model / threshold / template
///         ↓ overrides
///     global default (enricher.* GUC = EnricherConfig)  ── lowest
///
/// `enabled` cascades request → NS → global; model / score_threshold /
/// prompt_template_id cascade NS → global only (no request layer, topic 2.5).
///
/// Standalone (D3): this resolves whatever it is handed (the global EnricherConfig +
/// an NS blob + request params), producing the EnricherConfig the factory /
/// LlmEnricher use. Phasing of the live wiring that hands it those inputs:
///   - the request `enrich` arg from the SQL/HTTP layer is the Phase-1 per-request
///     override (§2.9 three-layer priority); wiring it in is D3.5 cross-Feature work.
///   - reading the NS blob from the NS-router cache (namespaces.enricher_config) is
///     PHASE-2 (F03 §2.7.bis Plan B: Phase-1 enricher connection is global via the
///     config.yaml enricher_llm role; per-NS api_key/model/budget is Phase 2). The
///     resolver is the Phase-2-ready pre-build — NOT a D3.5 wiring gap.
class EnricherConfigResolver {
public:
    explicit EnricherConfigResolver(const EnricherConfig& global) : global_(global) {}

    /// Resolve global ← ns ← request from the raw NS blob. "" / "{}" = inherit
    /// global. Returns CX_ERR (InvalidArgument) only when ns_json is present but
    /// not a JSON object; a malformed individual value is ignored (global
    /// fallback, never an error).
    Result<EnricherConfig> Resolve(const std::string& ns_json,
                                   const EnricherRequestParams& request = {}) const;

    /// Same, with an already-parsed NS config (skips JSON parsing).
    EnricherConfig Resolve(const EnricherNsConfig& ns,
                           const EnricherRequestParams& request = {}) const;

private:
    EnricherConfig global_;
};

}  // namespace cortrix::spc
