// Unit tests for the read-side decision-signal params (CRAG ?explain / query routing ?route /
// Doc summary ?granularity) on the query path:
//   - QueryRequest body parsing of explain / route / granularity (+ defaults).
//   - BuildExplainNode A/B/C phased-rollout gating (QUERY_CONTEXT_SPEC §3).
//   - The end-to-end marshalling the POST /api/v1/query handler performs: build a
//     QueryContext, run the FROZEN QueryComplexityClassifier::RouteAndUpdateContext
//     (Wave C-R2) honoring a ?route override, then dump the explain node + granularity.
//   - The query routing ?route invalid-token contract the handler pre-validates against.
//
// The query-string-wins precedence and the HTTP error bodies live in the route
// handler (anonymous-namespace helpers + httplib) and are exercised by
// tests/integration/test_query_routes.cpp; here we unit-test the building blocks the
// handler composes.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/query/query_request.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/query_context_explain.h"
#include "cortrix/query/query_complexity_classifier.h"
#include "cortrix/query/heuristic_complexity_backend.h"
#include "cortrix/query/complexity_config.h"

namespace cortrix {
namespace {

// ---------------- QueryRequest body parsing -----------------------------------

TEST(QueryExplainParamsTest, FromJson_ParsesExplainRouteGranularity) {
    json j = {
        {"query", "compare A and B"},
        {"namespace", "default"},
        {"explain", true},
        {"route", "complex"},
        {"granularity", "both"},
    };

    QueryRequest req;
    Status s = QueryRequest::FromJson(j, &req);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(req.explain);
    EXPECT_EQ(req.route, "complex");
    EXPECT_EQ(req.granularity, "both");
}

TEST(QueryExplainParamsTest, FromJson_DefaultsWhenAbsent) {
    // Absent params must keep safe defaults so behavior is unchanged (the additive
    // red-line): explain off, route/granularity "auto".
    json j = {
        {"query", "hello world"},
        {"namespace", "default"},
    };

    QueryRequest req;
    Status s = QueryRequest::FromJson(j, &req);
    ASSERT_TRUE(s.ok());
    EXPECT_FALSE(req.explain);
    EXPECT_EQ(req.route, "auto");
    EXPECT_EQ(req.granularity, "auto");
}

TEST(QueryExplainParamsTest, FromJson_IgnoresWrongTypedParams) {
    // Non-boolean explain / non-string route are ignored (defaults retained); the
    // route handler validates the enum, FromJson never rejects on these.
    json j = {
        {"query", "q"},
        {"namespace", "default"},
        {"explain", "yes"},     // wrong type → ignored
        {"route", 7},           // wrong type → ignored
        {"granularity", false}, // wrong type → ignored
    };

    QueryRequest req;
    Status s = QueryRequest::FromJson(j, &req);
    ASSERT_TRUE(s.ok());
    EXPECT_FALSE(req.explain);
    EXPECT_EQ(req.route, "auto");
    EXPECT_EQ(req.granularity, "auto");
}

// ---------------- BuildExplainNode A/B/C gating -------------------------------

TEST(QueryExplainNodeTest, AlwaysEmitsAandBClass) {
    query::QueryContext ctx;
    ctx.query = "q";
    ctx.ns_id = "ns-1";
    ctx.routing_path = "complex";
    ctx.complexity_score = 0.81f;
    ctx.routing_decision_source = "llm";

    nlohmann::json j = query::BuildExplainNode(ctx, /*include_debug=*/false);

    // A class (data integrity) — always present.
    for (const char* key : {"query", "ns_id", "multi_turn_context_warning",
                            "web_fallback_triggered"}) {
        EXPECT_TRUE(j.contains(key)) << "missing A-class key: " << key;
    }
    // B class (LLM path) — present because the node is built only under ?explain.
    for (const char* key : {"routing_path", "complexity_score",
                            "routing_decision_source", "chat_path_triggered",
                            "crag_verdict", "crag_score", "ambiguous_action_taken"}) {
        EXPECT_TRUE(j.contains(key)) << "missing B-class key: " << key;
    }
    EXPECT_EQ(j["routing_path"], "complex");
    EXPECT_NEAR(j["complexity_score"].get<float>(), 0.81f, 1e-4);
    EXPECT_EQ(j["routing_decision_source"], "llm");
}

TEST(QueryExplainNodeTest, OmitsCClassUnlessDebug) {
    query::QueryContext ctx;
    ctx.query = "q";
    ctx.crag_signals = {{"top1", 0.9f}};  // a value exists, but must stay hidden

    // include_debug=false → C class (crag_signals / routing_misclassified) omitted so
    // an Agent never mistakes a default/leftover signal for a triggered one (SPEC §3).
    nlohmann::json off = query::BuildExplainNode(ctx, /*include_debug=*/false);
    EXPECT_FALSE(off.contains("crag_signals"));
    EXPECT_FALSE(off.contains("routing_misclassified"));

    // include_debug=true → C class present.
    nlohmann::json on = query::BuildExplainNode(ctx, /*include_debug=*/true);
    EXPECT_TRUE(on.contains("crag_signals"));
    EXPECT_TRUE(on.contains("routing_misclassified"));
    EXPECT_NEAR(on["crag_signals"]["top1"].get<float>(), 0.9f, 1e-4);
}

TEST(QueryExplainNodeTest, DefaultCragFieldsEmittedNotOmitted) {
    // On the MVP query path no CRAG runs, so CRAG fields stay at SPEC defaults; they
    // must still appear (SPEC §6.3: default value reads as "not triggered").
    query::QueryContext ctx;
    ctx.query = "q";

    nlohmann::json j = query::BuildExplainNode(ctx, /*include_debug=*/false);
    EXPECT_EQ(j["crag_verdict"], "");
    EXPECT_NEAR(j["crag_score"].get<float>(), 0.0f, 1e-6);
    EXPECT_EQ(j["ambiguous_action_taken"], "");
    EXPECT_EQ(j["web_fallback_triggered"], false);
}

// ---------------- End-to-end marshalling (mirrors the handler) ----------------

// Build the explain object exactly as RegisterQueryRoutes does: init the context,
// run the frozen query routing router with the resolved ?route, dump, then echo granularity.
nlohmann::json MarshalExplain(const std::string& query,
                              const std::string& ns_id,
                              const std::string& route,
                              const std::string& granularity) {
    query::QueryContext qctx;
    qctx.query = query;
    qctx.ns_id = ns_id;

    query::QueryComplexityClassifier router(
        std::make_shared<query::HeuristicComplexityBackend>(),
        query::ComplexityConfig{});
    std::optional<std::string> route_override = route;
    router.RouteAndUpdateContext(qctx, route_override);

    const bool include_debug =
        qctx.routing_decision_source == "inference_failed_fallback";
    nlohmann::json node = query::BuildExplainNode(qctx, include_debug);
    node["granularity"] = granularity;
    return node;
}

TEST(QueryExplainMarshalTest, RouteOverrideForcesRoutingPath) {
    // ?route=simple is an explicit override → routing_decision_source="force_route".
    nlohmann::json node = MarshalExplain("anything at all", "ns-1", "simple", "auto");
    EXPECT_EQ(node["routing_path"], "simple");
    EXPECT_EQ(node["routing_decision_source"], "force_route");
    EXPECT_EQ(node["query"], "anything at all");
    EXPECT_EQ(node["ns_id"], "ns-1");
    EXPECT_EQ(node["granularity"], "auto");
}

TEST(QueryExplainMarshalTest, RouteAutoLetsClassifierDecideComplex) {
    // ?route=auto → the heuristic backend routes a multi-hop ("compare") query to
    // Complex via the classifier (source "llm").
    nlohmann::json node = MarshalExplain("compare X versus Y in detail", "ns-2",
                                         "auto", "chunk");
    EXPECT_EQ(node["routing_path"], "complex");
    EXPECT_EQ(node["routing_decision_source"], "llm");
    EXPECT_EQ(node["granularity"], "chunk");
}

TEST(QueryExplainMarshalTest, GranularityIsEchoedVerbatim) {
    for (const std::string g : {"auto", "chunk", "doc", "both"}) {
        nlohmann::json node = MarshalExplain("q", "ns", "auto", g);
        EXPECT_EQ(node["granularity"], g);
    }
}

// ---------------- query routing ?route validation contract --------------------

TEST(QueryExplainRouteValidationTest, RouterRejectsInvalidRouteToken) {
    // The handler pre-validates ?route and returns CX_ERR_ROUTER_FORCE_ROUTE_INVALID for
    // an out-of-set token; the frozen router enforces the same contract (ctx left
    // untouched, non-ok Status carrying the query routing identity).
    query::QueryContext ctx;
    ctx.query = "q";
    query::QueryComplexityClassifier router(
        std::make_shared<query::HeuristicComplexityBackend>(),
        query::ComplexityConfig{});

    std::optional<std::string> bad = std::string("sideways");
    Status s = router.RouteAndUpdateContext(ctx, bad);
    EXPECT_FALSE(s.ok());
    // ctx untouched (no routing committed) so the caller can surface the error.
    EXPECT_EQ(ctx.routing_path, "");
    EXPECT_EQ(ctx.routing_decision_source, "");
}

TEST(QueryExplainRouteValidationTest, RouterAcceptsAllLegalTokens) {
    for (const std::string route : {"auto", "simple", "complex", "chat"}) {
        query::QueryContext ctx;
        ctx.query = "a generic query";
        query::QueryComplexityClassifier router(
            std::make_shared<query::HeuristicComplexityBackend>(),
            query::ComplexityConfig{});
        std::optional<std::string> r = route;
        Status s = router.RouteAndUpdateContext(ctx, r);
        EXPECT_TRUE(s.ok()) << "route should be accepted: " << route;
    }
}

}  // namespace
}  // namespace cortrix
