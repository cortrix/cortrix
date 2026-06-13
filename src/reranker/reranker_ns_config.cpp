#include "cortrix/reranker/reranker_ns_config.h"

namespace cortrix::reranker {

Result<NsRerankerConfig> NsRerankerConfig::Parse(const std::string& json_blob) {
    NsRerankerConfig cfg;
    // "" / "{}" → inherit global entirely (topic 2.4). Treat empty as the empty
    // object so the common default-NS path needs no special-casing upstream.
    if (json_blob.empty()) return cfg;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_blob);
    } catch (const std::exception& e) {
        return Status::InvalidArgument(
            std::string("CX_ERR_INVALID_CONFIG_JSON: reranker_config parse: ") + e.what());
    }
    if (j.is_null()) return cfg;  // SQL NULL blob → inherit global
    if (!j.is_object()) {
        return Status::InvalidArgument(
            "CX_ERR_INVALID_CONFIG_JSON: reranker_config must be a JSON object");
    }

    // Each key OPTIONAL; a present key with the wrong JSON type is ignored
    // (defensive — never crash the query path on a bad NS value, degrade to the
    // global default). Unknown keys (e.g. Phase-2 model / score_threshold) ignored.
    if (auto it = j.find("enabled"); it != j.end() && it->is_boolean()) {
        cfg.enabled = it->get<bool>();
    }
    if (auto it = j.find("candidate_multiplier");
        it != j.end() && it->is_number_integer()) {
        cfg.candidate_multiplier = it->get<int>();
    }
    if (auto it = j.find("max_candidates"); it != j.end() && it->is_number_integer()) {
        cfg.max_candidates = it->get<int>();
    }
    return cfg;
}

}  // namespace cortrix::reranker
