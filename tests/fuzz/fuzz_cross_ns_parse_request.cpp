/// @file fuzz_cross_ns_parse_request.cpp
/// @brief libFuzzer harness for the cross-NS query body parse (F04 Sec.2.4).
///
/// Mirrors the cross-NS route: raw body -> json::parse (guarded) -> the cross-NS
/// ParseRequest logic (B' deprecation check + required `namespaces` array walk +
/// flat `filter` string-map copy).
///
/// WHY A LOCAL COPY (not the real TU): CrossNsQueryHandler::ParseRequest is a static
/// method, but it is *defined* in src/query/cross_ns_query_handler.cpp alongside
/// Handle(), which transitively links ScatterGather / CrossNsResponse / the whole
/// scatter-gather subsystem. Pulling that in for a 50-line parser is not worth it.
/// The body below is copied VERBATIM from cross_ns_query_handler.cpp::ParseRequest
/// (last synced when `timeout_ms` was added). DRIFT GUARD: if you change the real ParseRequest,
/// update this copy. The logic is intentionally byte-for-byte identical so the fuzzer
/// exercises the production control flow.
///
/// Build via build_fuzzers.sh.

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/status.h"
#include "cortrix/query/cross_ns_request.h"

using json = nlohmann::json;

namespace {

// Mirror of include/cortrix/query/cross_ns_query_handler.h:17.
constexpr char kCxErrDeprecatedField[] = "CX_ERR_DEPRECATED_FIELD";

// VERBATIM copy of CrossNsQueryHandler::ParseRequest (see file header drift guard).
cortrix::Status ParseRequest(const nlohmann::json& body,
                             cortrix::query::QueryRequest* out) {
    if (!body.is_object()) {
        return cortrix::Status::InvalidArgument("Request body must be a JSON object");
    }

    if (body.contains("namespace")) {
        return cortrix::Status::InvalidArgument(
            std::string(kCxErrDeprecatedField) +
            ": single 'namespace' field is deprecated, use 'namespaces' array");
    }

    if (body.contains("query") && body["query"].is_string()) {
        out->query = body["query"].get<std::string>();
    }
    if (out->query.empty()) {
        return cortrix::Status::InvalidArgument("'query' is required and must be non-empty");
    }

    if (!body.contains("namespaces") || !body["namespaces"].is_array()) {
        return cortrix::Status::InvalidArgument("'namespaces' is required and must be an array");
    }
    for (const auto& ns : body["namespaces"]) {
        if (!ns.is_string() || ns.get<std::string>().empty()) {
            return cortrix::Status::InvalidArgument("'namespaces' must be non-empty strings");
        }
        out->namespaces.push_back(ns.get<std::string>());
    }
    if (out->namespaces.empty()) {
        return cortrix::Status::InvalidArgument("'namespaces' must not be empty");
    }

    if (body.contains("top_k") && body["top_k"].is_number_integer()) {
        const long long wide = body["top_k"].get<long long>();
        if (wide < 1 || wide > 100) {
            return cortrix::Status::InvalidArgument(
                "'top_k' must be between 1 and 100");
        }
        out->top_k = static_cast<int>(wide);
    }
    if (body.contains("timeout_ms") && !body["timeout_ms"].is_null()) {
        const auto& t = body["timeout_ms"];
        const long long wide = t.is_number_integer() ? t.get<long long>() : 0;
        if (!t.is_number_integer() || wide < 100 || wide > 30000) {
            return cortrix::Status::InvalidArgument(
                "'timeout_ms' must be an integer between 100 and 30000");
        }
        out->timeout_ms = static_cast<int>(wide);
    }
    if (body.contains("rerank") && body["rerank"].is_boolean()) {
        out->rerank = body["rerank"].get<bool>();
    }
    if (body.contains("search_config") && body["search_config"].is_object()) {
        const auto& sc = body["search_config"];
        if (sc.contains("enable_vector") && sc["enable_vector"].is_boolean()) {
            out->search_config.enable_vector = sc["enable_vector"].get<bool>();
        }
        if (sc.contains("enable_bm25") && sc["enable_bm25"].is_boolean()) {
            out->search_config.enable_bm25 = sc["enable_bm25"].get<bool>();
        }
        if (sc.contains("enable_sparse") && sc["enable_sparse"].is_boolean()) {
            out->search_config.enable_sparse = sc["enable_sparse"].get<bool>();
        }
    }
    if (body.contains("filter") && body["filter"].is_object()) {
        for (auto it = body["filter"].begin(); it != body["filter"].end(); ++it) {
            if (it.value().is_string()) {
                out->filter[it.key()] = it.value().get<std::string>();
            }
        }
    }
    return cortrix::Status::Ok();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string body_str(reinterpret_cast<const char*>(data), size);

    json body;
    try {
        body = json::parse(body_str);
    } catch (...) {
        return 0;  // malformed JSON => handled 400 upstream.
    }

    cortrix::query::QueryRequest out;
    cortrix::Status st = ParseRequest(body, &out);
    (void)st;
    return 0;
}
