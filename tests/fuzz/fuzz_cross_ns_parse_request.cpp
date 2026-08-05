/// @file fuzz_cross_ns_parse_request.cpp
/// @brief libFuzzer harness for the cross-NS query body parse (cross-NS query Sec.2.4).
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
/// (commit release/v1.0.0-rc.1). DRIFT GUARD: if you change the real ParseRequest,
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
        out->top_k = body["top_k"].get<int>();
    }
    if (body.contains("rerank") && body["rerank"].is_boolean()) {
        out->rerank = body["rerank"].get<bool>();
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
