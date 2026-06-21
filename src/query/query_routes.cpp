#include "cortrix/query/query_routes.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/query_response.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/query_context_explain.h"
#include "cortrix/query/query_complexity_classifier.h"
#include "cortrix/query/heuristic_complexity_backend.h"
#include "cortrix/query/complexity_config.h"
#include "cortrix/query/router_error.h"
#include "cortrix/agent_friendly/error.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/logging/logging.h"
#include "httplib.h"

#include <memory>
#include <optional>
#include <string>

namespace cortrix {

namespace {

// Read-side decision-signal params (F37 ?explain / F39 ?route / F41 ?granularity).
// Precedence: a query-string value overrides the JSON body value (briefing red-line
// "if both, query-string wins"). The body value has already been parsed into
// `req` by QueryRequest::FromJson before these run.

/// Apply the query-string `explain` override. Accepts the Agent-friendly truthy
/// tokens "true"/"1" (case-insensitive "true"); any other present value is false.
void ApplyExplainParam(const httplib::Request& http_req, QueryRequest& req) {
    if (!http_req.has_param("explain")) return;
    const std::string v = http_req.get_param_value("explain");
    req.explain = (v == "true" || v == "TRUE" || v == "True" || v == "1");
}

/// Apply the query-string `route` / `granularity` overrides (raw; validated below).
void ApplyRouteGranularityParams(const httplib::Request& http_req, QueryRequest& req) {
    if (http_req.has_param("route")) {
        req.route = http_req.get_param_value("route");
    }
    if (http_req.has_param("granularity")) {
        req.granularity = http_req.get_param_value("granularity");
    }
}

bool IsValidRoute(const std::string& r) {
    return r == "auto" || r == "simple" || r == "complex" || r == "chat";
}

bool IsValidGranularity(const std::string& g) {
    return g == "auto" || g == "chunk" || g == "doc" || g == "both";
}

}  // namespace

void RegisterQueryRoutes(
    httplib::Server& svr,
    ApiKeyAuth& auth,
    cortrix::resource::INamespacePool& pool,
    OnnxEmbedder& embedder,
    IntentClassifier& classifier,
    RRFFusion& fusion) {

    svr.Post("/api/v1/query", WithAuth(auth, kPermRead,
        [&](const httplib::Request& req, httplib::Response& res,
            const RequestContext& ctx) {

            // Step 1: Parse JSON body
            json body;
            try {
                body = json::parse(req.body);
            } catch (...) {
                WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"), ctx.request_id);
                return;
            }

            QueryRequest query_req;
            Status parse_status = QueryRequest::FromJson(body, &query_req);
            if (!parse_status.ok()) {
                WriteJsonError(res, parse_status, ctx.request_id);
                return;
            }

            // Step 1b: Layer query-string params over the body (query-string wins)
            // for the read-side decision-signal controls, then validate the enums.
            ApplyExplainParam(req, query_req);
            ApplyRouteGranularityParams(req, query_req);

            // F39 ?route enum (§4.3): an invalid token returns the Agent-friendly
            // CX_ERR_F39_FORCE_ROUTE_INVALID body with structured_data.invalid_route_value
            // (machine-readable per CLAUDE.md §5 / GEN-Agent #1,#5), not a generic
            // parse error. "auto" means "no override; let the router decide".
            if (!IsValidRoute(query_req.route)) {
                auto err = cortrix::query::MakeRouterError(
                    cortrix::query::RouterErrorCode::kForceRouteInvalid,
                    {{"invalid_route_value", query_req.route}});
                // Match the project convention: the Agent-friendly body nests under a
                // top-level "error" key (see operations_routes / tenant_routes /
                // cross_ns_query_handler).
                json error_body;
                error_body["error"] = cortrix::agent_friendly::ToJson(err);
                res.status = 400;
                res.set_header("X-Request-Id", ctx.request_id);
                res.set_content(error_body.dump(), "application/json");
                return;
            }

            // F41 ?granularity enum (§6.2): auto|chunk|doc|both. The frozen F41 error
            // set (CX_ERR_F41_*) has no request-param identity, so an invalid value is
            // a generic InvalidArgument (consistent with the other request-shape
            // validations in QueryRequest::Validate).
            if (!IsValidGranularity(query_req.granularity)) {
                WriteJsonError(res, Status::InvalidArgument(
                    "granularity must be one of: auto, chunk, doc, both"),
                    ctx.request_id);
                return;
            }

            // Step 2: Normalize then Validate
            // Design spec: Normalize() MUST be called before Validate().
            // Normalize() truncates queries exceeding kMaxQueryLength (2000 chars)
            // instead of rejecting them. This matches ARCHITECTURE_DEEP_DESIGN.md
            // Section 3.5.4 boundary #13: "truncate to max_query_length, not reject".
            query_req.Normalize();

            Status validate_status = query_req.Validate();
            if (!validate_status.ok()) {
                WriteJsonError(res, validate_status, ctx.request_id);
                return;
            }

            // Step 3: Namespace access check via the runtime PermissionService
            // seam (ARCHITECTURE V6). Anti-enumeration (F04 issue 2.6):
            // unauthorized and not-found share CX_ERR_NS_UNAUTHORIZED, and the
            // namespace name is never echoed back (no existence oracle).
            if (!auth.Authorize(ctx.auth, query_req.namespace_name, kPermRead).ok()) {
                WriteJsonError(res, Status::PermissionDenied("CX_ERR_NS_UNAUTHORIZED"),
                               ctx.request_id);
                return;
            }

            // Step 4: Acquire namespace (per-request façade over the F05 pool;
            // RAII-released when this handler scope exits — D-I5a per-request).
            cortrix::resource::NamespaceFacade facade(pool, query_req.namespace_name);
            Status acq = facade.Acquire();
            if (!acq.ok()) {
                WriteJsonError(res, Status::NotFound(
                    "Namespace '" + query_req.namespace_name + "' not found: " + acq.message()),
                    ctx.request_id);
                return;
            }

            // Step 5: Construct per-request components
            VectorSearcher vec_searcher(facade.vec_index(), embedder);
            BM25Searcher bm25_searcher(facade.store());
            PostFilter post_filter(facade.store());
            DegradationManager deg_mgr;
            SqlExecutorStub sql_stub;  // MVP: always returns kSkipped

            // Step 6: Build and execute pipeline
            QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                                   classifier, post_filter, deg_mgr, sql_stub);
            QueryResponse response = pipeline.Execute(query_req);

            // Step 7: Check degradation level
            if (response.meta.degradation_level == static_cast<int>(DegradationLevel::kL3)) {
                WriteJsonError(res, Status::Unavailable("All search routes failed"), ctx.request_id);
                return;
            }

            // Step 8: Build response JSON, attaching the ?explain QueryContext node.
            json response_json = response.ToJson();

            if (query_req.explain) {
                // Populate a QueryContext for the read-side explain dump. The routing
                // (F39) signals come from the FROZEN QueryComplexityClassifier
                // (RouteAndUpdateContext, Wave C-R2) — the route handler is the read
                // side that surfaces the already-implemented decision logic; it adds
                // no new routing behavior. Standalone backend is the heuristic guard
                // (real DistilBERT-tiny ONNX wiring is D3.5); QueryComplexityClassifier
                // is total (never throws). The granularity is echoed so the Agent sees
                // the resolved value; F37 CRAG fields stay at their SPEC defaults
                // (""/0.0/{}) because no CRAG evaluation runs on this MVP path — per
                // QUERY_CONTEXT_SPEC §6.3 a default value reads as "not triggered".
                cortrix::query::QueryContext qctx;
                qctx.query = query_req.query;
                qctx.ns_id = query_req.namespace_name;

                cortrix::query::QueryComplexityClassifier router(
                    std::make_shared<cortrix::query::HeuristicComplexityBackend>(),
                    cortrix::query::ComplexityConfig{});
                // The validated route ("auto" = let the classifier decide) drives the
                // §6.1 override step; an out-of-set token was already rejected above,
                // so this never returns the force-route-invalid status here.
                std::optional<std::string> route_override = query_req.route;
                router.RouteAndUpdateContext(qctx, route_override);

                // C-class debug fields are exposed only when a capability ran AND
                // failed (SPEC §3). On this path the only such signal is the router's
                // transient-inference fallback; include the C block then.
                const bool include_debug =
                    qctx.routing_decision_source == "inference_failed_fallback";

                response_json["explain"] =
                    cortrix::query::BuildExplainNode(qctx, include_debug);
                response_json["explain"]["granularity"] = query_req.granularity;
            }

            res.status = 200;
            res.set_header("X-Request-Id", ctx.request_id);
            res.set_content(response_json.dump(), "application/json");
        }
    ));
}

}  // namespace cortrix
