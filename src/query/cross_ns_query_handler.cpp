#include "cortrix/query/cross_ns_query_handler.h"

#include <utility>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/query/cross_ns_error.h"

namespace cortrix::query {

namespace {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

// Wrap one Agent-friendly error into the §2.6 body: {"error": {...}}.
nlohmann::json ErrorBody(const AgentFriendlyError& err) {
    nlohmann::json j;
    j["error"] = agent_friendly::ToJson(err);
    return j;
}

// The CX_ERR_DEPRECATED_FIELD error (§2.4 / topic 4.1 B'). 400, permanent, with
// structured_data telling the Agent exactly which field to drop and what to use.
AgentFriendlyError DeprecatedFieldError() {
    AgentFriendlyError err;
    err.code = kCxErrDeprecatedField;
    err.message =
        "Field 'namespace' is deprecated, use 'namespaces' array instead";
    err.retryable = false;
    err.category = ErrorCategory::kPermanent;
    err.structured_data = nlohmann::json{{"deprecated_field", "namespace"},
                                         {"use_instead", "namespaces"}};
    return err;
}

// A generic 400 bad-request Agent-friendly error (malformed/empty request body).
AgentFriendlyError BadRequestError(const std::string& message) {
    AgentFriendlyError err;
    err.code = "CX_ERR_BAD_REQUEST";
    err.message = message;
    err.retryable = false;
    err.category = ErrorCategory::kPermanent;
    err.structured_data = nlohmann::json::object();
    return err;
}

}  // namespace

Status CrossNsQueryHandler::ParseRequest(const nlohmann::json& body,
                                         QueryRequest* out) {
    if (!body.is_object()) {
        return Status::InvalidArgument("Request body must be a JSON object");
    }

    // B' topic 4.1: the MVP single `namespace` field is not accepted. Detect it FIRST
    // (before reading `namespaces`) so a request carrying it always gets the precise
    // deprecation error, even if it ALSO sends the new array. The token prefix lets
    // Handle() route to the CX_ERR_DEPRECATED_FIELD body.
    if (body.contains("namespace")) {
        return Status::InvalidArgument(
            std::string(kCxErrDeprecatedField) +
            ": single 'namespace' field is deprecated, use 'namespaces' array");
    }

    if (body.contains("query") && body["query"].is_string()) {
        out->query = body["query"].get<std::string>();
    }
    if (out->query.empty()) {
        return Status::InvalidArgument("'query' is required and must be non-empty");
    }

    // `namespaces` — required array of non-empty strings (["*"] allowed = wildcard).
    if (!body.contains("namespaces") || !body["namespaces"].is_array()) {
        return Status::InvalidArgument("'namespaces' is required and must be an array");
    }
    for (const auto& ns : body["namespaces"]) {
        if (!ns.is_string() || ns.get<std::string>().empty()) {
            return Status::InvalidArgument("'namespaces' must be non-empty strings");
        }
        out->namespaces.push_back(ns.get<std::string>());
    }
    if (out->namespaces.empty()) {
        return Status::InvalidArgument("'namespaces' must not be empty");
    }

    // Optional: top_k (default 10), rerank (default true), filter (flat string map).
    if (body.contains("top_k") && body["top_k"].is_number_integer()) {
        out->top_k = body["top_k"].get<int>();
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
    return Status::Ok();
}

HandlerResult CrossNsQueryHandler::Handle(const nlohmann::json& body,
                                          const AuthContext& auth,
                                          const QueryContext* routing_ctx) {
    // 1. Parse + B' deprecation + validation (§2.4).
    QueryRequest request;
    Status parsed = ParseRequest(body, &request);
    if (!parsed.ok()) {
        const std::string& msg = parsed.message();
        // The deprecated-field path is tagged by its token prefix.
        if (msg.rfind(kCxErrDeprecatedField, 0) == 0) {
            return HandlerResult{400, ErrorBody(DeprecatedFieldError())};
        }
        return HandlerResult{400, ErrorBody(BadRequestError(msg))};
    }

    // 2. Run the scatter. Hard failures (auth / too-many / unauthorized) throw
    //    CrossNsException → serialize GetError() with the §3.1 HTTP status. The
    //    overall scatter-timeout partial case does NOT throw — it returns a 200
    //    response whose .error is set (§2.7 principle 3), handled below. The
    //    optional routing_ctx carries the resolved route into the per-NS
    //    executors; when null ScatterGather builds the context itself (unchanged).
    try {
        CrossNsResponse resp = scatter_->Execute(request, auth, routing_ctx);
        return HandlerResult{200, resp.ToJson()};
    } catch (const CrossNsException& e) {
        const int http = GetCrossNsErrorInfo(e.code()).http_status;
        return HandlerResult{http, ErrorBody(e.GetError())};
    } catch (const agent_friendly::AgentFriendlyException& e) {
        // Any other Agent-friendly boundary error → 400 (defensive; ScatterGather's
        // hard failures are all CrossNsException).
        return HandlerResult{400, ErrorBody(e.GetError())};
    }
}

}  // namespace cortrix::query
