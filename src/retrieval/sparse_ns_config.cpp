#include "cortrix/retrieval/sparse_ns_config.h"

#include <algorithm>

namespace cortrix::retrieval {

Result<NsSparseConfig> NsSparseConfig::Parse(const std::string& json_blob) {
    NsSparseConfig cfg;
    // "" → inherit global entirely. Treat empty as the empty object so the
    // common default-NS path needs no special-casing upstream.
    if (json_blob.empty()) return cfg;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_blob);
    } catch (const std::exception& e) {
        return Status::InvalidArgument(
            std::string("CX_ERR_INVALID_CONFIG_JSON: sparse_config parse: ") +
            e.what());
    }
    if (j.is_null()) return cfg;  // SQL NULL blob → inherit global
    if (!j.is_object()) {
        return Status::InvalidArgument(
            "CX_ERR_INVALID_CONFIG_JSON: sparse_config must be a JSON object");
    }

    // sparse_top_k OPTIONAL; a present key with the wrong JSON type is ignored
    // (defensive — never crash the query path on a bad NS value). Range is
    // enforced at Resolve (a stored out-of-bounds value still loads, then clamps).
    if (auto it = j.find("sparse_top_k");
        it != j.end() && it->is_number_integer()) {
        cfg.sparse_top_k = it->get<int>();
    }
    return cfg;
}

int ClampSparseTopK(int requested) {
    return std::clamp(requested, kSparseTopKMin, kSparseTopKMax);
}

int ResolveSparseTopK(const NsSparseConfig& ns_cfg, const IGlobalConfig* global) {
    // 1. NS override wins when present.
    if (ns_cfg.sparse_top_k.has_value()) {
        return ClampSparseTopK(*ns_cfg.sparse_top_k);
    }
    // 2. Else the global default (generic IGlobalConfig key), else compile-time.
    int base = kSparseTopKDefault;
    if (global != nullptr) {
        Result<int> r = global->GetInt(kSparseTopKConfigKey);
        if (r.ok()) base = r.value();
    }
    return ClampSparseTopK(base);
}

}  // namespace cortrix::retrieval
