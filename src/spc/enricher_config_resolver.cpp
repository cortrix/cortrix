#include "cortrix/spc_enricher/enricher_config_resolver.h"

#include <nlohmann/json.hpp>

namespace cortrix::spc {

Result<EnricherNsConfig> EnricherNsConfig::Parse(const std::string& json_blob) {
    EnricherNsConfig cfg;
    // "" / "{}" → inherit global entirely (topic 2.4).
    if (json_blob.empty()) return cfg;

    nlohmann::json j = nlohmann::json::parse(json_blob, /*cb=*/nullptr,
                                             /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        return Status::InvalidArgument(
            "CX_ERR_INVALID_CONFIG_JSON: enricher_config parse failed");
    }
    if (j.is_null()) return cfg;  // SQL NULL blob → inherit global
    if (!j.is_object()) {
        return Status::InvalidArgument(
            "CX_ERR_INVALID_CONFIG_JSON: enricher_config must be a JSON object");
    }

    // Each key OPTIONAL; a present key with the wrong JSON type is ignored
    // (defensive — never crash the SPC path on a bad NS value, degrade to global).
    if (auto it = j.find("enabled"); it != j.end() && it->is_boolean()) {
        cfg.enabled = it->get<bool>();
    }
    if (auto it = j.find("model"); it != j.end() && it->is_string()) {
        cfg.model = it->get<std::string>();
    }
    if (auto it = j.find("score_threshold"); it != j.end() && it->is_number()) {
        cfg.score_threshold = it->get<float>();
    }
    if (auto it = j.find("prompt_template_id"); it != j.end() && it->is_string()) {
        cfg.prompt_template_id = it->get<std::string>();
    }
    return cfg;
}

EnricherConfig EnricherConfigResolver::Resolve(const EnricherNsConfig& ns,
                                               const EnricherRequestParams& request) const {
    EnricherConfig eff = global_;  // start from global resident defaults

    // B-class fields cascade NS → global (no request layer, topic 2.5).
    if (ns.enabled) eff.enabled = *ns.enabled;
    if (ns.model) eff.model = *ns.model;
    if (ns.score_threshold) eff.score_threshold = *ns.score_threshold;
    if (ns.prompt_template_id) eff.prompt_template_id = *ns.prompt_template_id;

    // `enabled` request layer is highest (only sets enabled). This is the
    // example: NS {"enabled":false} + request enrich=true → enabled.
    if (request.enrich) eff.enabled = *request.enrich;

    return eff;
}

Result<EnricherConfig> EnricherConfigResolver::Resolve(
    const std::string& ns_json, const EnricherRequestParams& request) const {
    Result<EnricherNsConfig> ns = EnricherNsConfig::Parse(ns_json);
    if (!ns.ok()) return ns.status();
    return Resolve(ns.value(), request);
}

}  // namespace cortrix::spc
