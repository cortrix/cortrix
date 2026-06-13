#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_context.h"
#include "cortrix/common/status.h"
#include "cortrix/query/cross_ns_request.h"
#include "cortrix/query/scatter_gather.h"

namespace cortrix::query {

/// CX_ERR_DEPRECATED_FIELD — the B' incompatible-with-MVP error (F04 §2.4 / topic 4.1).
/// NOT one of the 6 CrossNsErrorCode identities (those are query-execution errors);
/// this is a request-validation error, so it is its own stable code. category =
/// permanent, retryable = false, structured_data = {deprecated_field, use_instead}.
inline constexpr char kCxErrDeprecatedField[] = "CX_ERR_DEPRECATED_FIELD";

/// The HTTP-shaped result of handling a POST /api/v1/query request (S4.1). `status`
/// is the HTTP status; `body` is the JSON to serialize. Hard auth/quota failures
/// surface as their §3.1 status (401/403/400); partial scatter timeout is 200 with a
/// partial body (§2.7 principle 3); a normal response is 200.
struct HandlerResult {
    int status = 200;
    nlohmann::json body;
};

/// CrossNsQueryHandler — the POST /api/v1/query handler **logic** (F04 §2.4 / §4.1 /
/// S4.1). Parses the cross-NS QueryRequest, rejects the deprecated MVP `namespace`
/// single field (CX_ERR_DEPRECATED_FIELD), validates the request, then runs
/// ScatterGather and serializes the §2.5 response / §2.6 error body.
///
/// 🚩 D3.5 FLAG: this is the handler logic only. Mounting it on the live HTTP server
/// route table (httplib/Drogon binding, request-context plumbing, the
/// AuthMiddleware-populated AuthContext) is **D3.5**. Standalone it is driven
/// directly with a parsed JSON body + an AuthContext, so the parse → deprecation →
/// validate → scatter → serialize pipeline is fully unit-testable now.
class CrossNsQueryHandler {
public:
    /// @param scatter  the ScatterGather to run the query (NOT owned).
    explicit CrossNsQueryHandler(ScatterGather* scatter) : scatter_(scatter) {}

    /// Handle one request body + auth. Never throws — every failure path becomes a
    /// HandlerResult with the right status + an Agent-friendly error body.
    ///
    /// @param routing_ctx  optional pre-built QueryContext carrying the F39 routing
    ///                    decision (routing_path etc., Q3 wiring). When non-null it is
    ///                    threaded into ScatterGather.Execute so the per-NS executors
    ///                    see the resolved route; when null, ScatterGather builds the
    ///                    context from the request + auth (unchanged standalone path).
    HandlerResult Handle(const nlohmann::json& body, const AuthContext& auth,
                         const QueryContext* routing_ctx = nullptr);

    /// Parse a cross-NS QueryRequest from a JSON body (§2.4 schema). Returns
    /// InvalidArgument with a message starting `CX_ERR_DEPRECATED_FIELD` when the
    /// deprecated single `namespace` field is present (topic 4.1 B'), or a plain
    /// InvalidArgument for a malformed/empty request. The token in the message lets
    /// Handle() pick the right error body. Exposed for unit testing.
    static Status ParseRequest(const nlohmann::json& body, QueryRequest* out);

private:
    ScatterGather* scatter_;
};

}  // namespace cortrix::query
