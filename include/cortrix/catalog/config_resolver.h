#pragma once
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/catalog_error.h"
#include "cortrix/common/result.h"

namespace cortrix::catalog {

/// Three-layer config resolution (F12 §3.8 / §6.1): the effective config for a
/// Feature is global default ← NS override ← request override, where the request
/// layer may only set whitelisted fields (§3.5/§6.2). Generic over any config
/// struct `ConfigT` that is nlohmann-JSON serializable (has to_json/from_json) —
/// merging happens at the JSON object layer, so no reflection is needed.
///
/// Override semantics: object keys present in a higher layer replace the lower
/// layer's value (shallow merge on top-level keys). NS config is the parsed
/// `namespaces.<feature>_config` blob ('{}' = no override = inherit global).
///
/// Not a cache itself — INSRouter holds the §6.3 NS-metadata cache; this resolves
/// against whatever NS JSON it is handed.
template <typename ConfigT>
class ConfigResolver {
public:
    /// Restrict which top-level keys a request-level override may set (§3.5
    /// strict whitelist). Keys outside the set are ignored in `request`. Empty
    /// set (default) = no request-level overrides honored.
    void SetRequestAllowedFields(const std::set<std::string>& fields) {
        request_allowed_ = fields;
    }

    /// Resolve global ← ns_json ← (whitelisted) request → final ConfigT.
    /// `ns_json` is the raw NS config blob ("" or "{}" = inherit global). A
    /// non-null `request` contributes only its whitelisted keys. Returns
    /// CX_ERR_INVALID_CONFIG_JSON if ns_json is present but not a JSON object,
    /// or if the merged result fails to deserialize into ConfigT.
    Result<ConfigT> Resolve(const ConfigT& global,
                            const std::string& ns_json,
                            const ConfigT* request = nullptr) const {
        nlohmann::json merged = nlohmann::json(global);  // to_json(global)

        // Layer 2: NS override.
        if (!ns_json.empty()) {
            nlohmann::json ns;
            try {
                ns = nlohmann::json::parse(ns_json);
            } catch (const std::exception& e) {
                return CatalogStatus(CatalogErrorCode::kInvalidConfigJson,
                                     std::string("ns config parse: ") + e.what());
            }
            if (!ns.is_null()) {
                if (!ns.is_object()) {
                    return CatalogStatus(CatalogErrorCode::kInvalidConfigJson,
                                         "ns config must be a JSON object");
                }
                for (auto it = ns.begin(); it != ns.end(); ++it) {
                    merged[it.key()] = it.value();
                }
            }
        }

        // Layer 3: request override, whitelisted keys only.
        if (request != nullptr && !request_allowed_.empty()) {
            nlohmann::json req = nlohmann::json(*request);
            for (const std::string& key : request_allowed_) {
                if (req.contains(key)) merged[key] = req[key];
            }
        }

        // Deserialize the merged object back into ConfigT.
        try {
            return merged.get<ConfigT>();
        } catch (const std::exception& e) {
            return CatalogStatus(CatalogErrorCode::kInvalidConfigJson,
                                 std::string("merged config deserialize: ") + e.what());
        }
    }

    const std::set<std::string>& request_allowed_fields() const {
        return request_allowed_;
    }

private:
    std::set<std::string> request_allowed_;
};

}  // namespace cortrix::catalog
