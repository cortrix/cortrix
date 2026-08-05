#include "cortrix/query/query_request.h"
#include <regex>

namespace cortrix {

Status QueryRequest::FromJson(const json& j, QueryRequest* req) {
    if (!j.is_object()) {
        return Status::InvalidArgument("Invalid JSON in request body");
    }

    // Required: query
    if (j.contains("query") && j["query"].is_string()) {
        req->query = j["query"].get<std::string>();
    }

    // Required: namespace
    if (j.contains("namespace") && j["namespace"].is_string()) {
        req->namespace_name = j["namespace"].get<std::string>();
    }

    // Optional: top_k
    if (j.contains("top_k") && j["top_k"].is_number_integer()) {
        req->top_k = j["top_k"].get<int>();
    }

    // Optional: timeout_ms
    if (j.contains("timeout_ms") && j["timeout_ms"].is_number_integer()) {
        req->timeout_ms = j["timeout_ms"].get<int>();
    }

    // Optional: filters
    if (j.contains("filters") && j["filters"].is_object()) {
        const auto& f = j["filters"];
        if (f.contains("block_type") && f["block_type"].is_array()) {
            for (const auto& bt : f["block_type"]) {
                if (bt.is_string()) {
                    req->filters.block_types.push_back(bt.get<std::string>());
                }
            }
        }
        if (f.contains("exclude_types") && f["exclude_types"].is_array()) {
            for (const auto& et : f["exclude_types"]) {
                if (et.is_string()) {
                    req->filters.exclude_types.push_back(et.get<std::string>());
                }
            }
        }
        if (f.contains("source_path") && f["source_path"].is_string()) {
            req->filters.source_path = f["source_path"].get<std::string>();
        }
        if (f.contains("created_after") && f["created_after"].is_string()) {
            req->filters.created_after = f["created_after"].get<std::string>();
        }
        if (f.contains("doc_id") && f["doc_id"].is_string()) {
            req->filters.doc_id = f["doc_id"].get<std::string>();
        }
    }

    // Optional: explain (F37/F39 ?explain). Body form; query-string form is layered
    // on top by the route handler (query-string wins). Accept only a JSON boolean.
    if (j.contains("explain") && j["explain"].is_boolean()) {
        req->explain = j["explain"].get<bool>();
    }

    // Optional: route (F39 override) / granularity. Body form only here; the
    // enum is validated at the route boundary so an invalid value can surface the
    // Agent-friendly CX_ERR_F39_* body (route) instead of a generic parse error.
    if (j.contains("route") && j["route"].is_string()) {
        req->route = j["route"].get<std::string>();
    }
    if (j.contains("granularity") && j["granularity"].is_string()) {
        req->granularity = j["granularity"].get<std::string>();
    }

    // Optional: search_config
    if (j.contains("search_config") && j["search_config"].is_object()) {
        const auto& sc = j["search_config"];
        if (sc.contains("enable_vector") && sc["enable_vector"].is_boolean()) {
            req->search_config.enable_vector = sc["enable_vector"].get<bool>();
        }
        if (sc.contains("enable_bm25") && sc["enable_bm25"].is_boolean()) {
            req->search_config.enable_bm25 = sc["enable_bm25"].get<bool>();
        }
        if (sc.contains("enable_sql") && sc["enable_sql"].is_boolean()) {
            req->search_config.enable_sql = sc["enable_sql"].get<bool>();
        }
    }

    return Status::Ok();
}

bool QueryRequest::Normalize() {
    // Design spec (ARCHITECTURE_DEEP_DESIGN.md 3.5.4 boundary #13):
    // "queries > 10000 chars -> truncate to max_query_length (config default 2000), warning log"
    // We truncate any query exceeding kMaxQueryLength to that limit.
    was_truncated = false;
    if (query.size() > kMaxQueryLength) {
        query.resize(kMaxQueryLength);
        was_truncated = true;
    }
    return was_truncated;
}

Status QueryRequest::Validate() const {
    // query: required, non-empty
    // Note: query length is enforced by Normalize() (truncation to kMaxQueryLength),
    // not by rejection.  This matches the design spec which says "truncate, not reject".
    if (query.empty()) {
        return Status::InvalidArgument("query is required");
    }

    // namespace_name: required, match [a-zA-Z0-9_-]{1,64}
    if (namespace_name.empty()) {
        return Status::InvalidArgument("namespace is required");
    }
    static const std::regex ns_pattern("^[a-zA-Z0-9_-]{1,64}$");
    if (!std::regex_match(namespace_name, ns_pattern)) {
        return Status::InvalidArgument("invalid namespace format");
    }

    // top_k: 1 <= top_k <= 100
    if (top_k < 1 || top_k > 100) {
        return Status::InvalidArgument("top_k must be between 1 and 100");
    }

    // timeout_ms: 100 <= timeout_ms <= 30000
    if (timeout_ms < 100 || timeout_ms > 30000) {
        return Status::InvalidArgument("timeout_ms must be between 100 and 30000");
    }

    // block_types: each must be valid
    for (const auto& bt : filters.block_types) {
        if (BlockTypeFromString(bt) < 0) {
            return Status::InvalidArgument("invalid block_type: " + bt);
        }
    }

    // exclude_types: each must be valid
    for (const auto& et : filters.exclude_types) {
        if (BlockTypeFromString(et) < 0) {
            return Status::InvalidArgument("invalid exclude_type: " + et);
        }
    }

    return Status::Ok();
}

}  // namespace cortrix
