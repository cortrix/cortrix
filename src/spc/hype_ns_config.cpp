#include "cortrix/spc/hype_ns_config.h"

#include <algorithm>

namespace cortrix::spc {

Result<NsHyPEConfig> NsHyPEConfig::Parse(const std::string& json_blob) {
    NsHyPEConfig cfg;
    if (json_blob.empty()) return cfg;  // inherit global entirely

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_blob);
    } catch (const std::exception& e) {
        return Status::InvalidArgument(
            std::string("CX_ERR_INVALID_CONFIG_JSON: hype_config parse: ") +
            e.what());
    }
    if (j.is_null()) return cfg;  // SQL NULL blob → inherit global
    if (!j.is_object()) {
        return Status::InvalidArgument(
            "CX_ERR_INVALID_CONFIG_JSON: hype_config must be a JSON object");
    }

    // questions_per_chunk OPTIONAL; wrong-type ignored (defensive). Range enforced
    // at Resolve (a stored out-of-bounds value still loads, then clamps).
    if (auto it = j.find("questions_per_chunk");
        it != j.end() && it->is_number_integer()) {
        cfg.questions_per_chunk = it->get<int>();
    }
    return cfg;
}

int ClampHypeK(int requested) {
    return std::clamp(requested, kHypeQuestionsMin, kHypeQuestionsMax);
}

int ResolveHypeK(const NsHyPEConfig& ns_cfg, const IGlobalConfig* global) {
    // 1. NS override wins when present.
    if (ns_cfg.questions_per_chunk.has_value()) {
        return ClampHypeK(*ns_cfg.questions_per_chunk);
    }
    // 2. Else the global default (generic IGlobalConfig key), else compile-time.
    int base = kHypeQuestionsDefault;
    if (global != nullptr) {
        Result<int> r = global->GetInt(kHypeQuestionsPerChunkKey);
        if (r.ok()) base = r.value();
    }
    return ClampHypeK(base);
}

}  // namespace cortrix::spc
