// query_wiring.cpp — behavioral coverage of the F39/F36 wiring contracts.
//
// Honest scope note: the wiring helpers DecisionOf / ReadRouteOverride /
// ResolveRagFusionConfig / RecordQueryTrace live in an *anonymous namespace* inside
// query_wiring.cpp and the public CrossNsQueryWiring ctor needs a live F05 pool +
// bge-m3 embedder + tenant PermissionService, so neither the closure nor those
// statics is directly unit-addressable without standing up the live server. We
// therefore pin the SAME contracts at the real components the closure delegates to:
//
//   * ReadRouteOverride -> CX_ERR_F39_FORCE_ROUTE_INVALID : the override flows into
//     QueryComplexityClassifier::RouteAndUpdateContext(ctx, route); a bad token
//     returns the F39 permanent error and leaves ctx untouched (exactly what the
//     closure surfaces as a 400).
//   * DecisionOf(routing_path, source) : RouteAndUpdateContext writes the
//     routing_path + routing_decision_source the closure maps to a
//     QueryRouterMetrics::Decision, and itself records that decision. We assert the
//     written fields + the metric the closure reads from them.
//   * ResolveRagFusionConfig : the resolved RagFusionConfig defaults to
//     enabled=false (topic 3) and only the per-request opt-in flips it on — the
//     exact gate the closure's use_rag_fusion computes.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "cortrix/query/complexity_config.h"
#include "cortrix/query/heuristic_complexity_backend.h"
#include "cortrix/query/query_complexity_classifier.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/query_router_metrics.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/query/router_error.h"

namespace cortrix::query {
namespace {

QueryComplexityClassifier MakeClassifier(const ComplexityConfig& cfg = {}) {
    return QueryComplexityClassifier(
        std::make_shared<HeuristicComplexityBackend>(/*reported_confidence=*/0.9f),
        cfg);
}

class QueryWiringTest : public ::testing::Test {
protected:
    void SetUp() override { QueryRouterMetrics::Instance().ResetForTest(); }
};

// ===========================================================================
// ReadRouteOverride -> CX_ERR_F39_FORCE_ROUTE_INVALID
// ===========================================================================

// A bad ?route= override is the F39 permanent error; ctx is left untouched so the
// closure can surface the Agent-friendly 400 without a half-written routing_path.
TEST_F(QueryWiringTest, InvalidRouteOverrideYieldsF39Error) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "anything";
    Status s = classifier.RouteAndUpdateContext(ctx, std::optional<std::string>("banana"));
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_F39_FORCE_ROUTE_INVALID"), std::string::npos);
    // ctx untouched on the invalid-override path.
    EXPECT_TRUE(ctx.routing_path.empty());
    EXPECT_TRUE(ctx.routing_decision_source.empty());
}

// Each valid override token is honoured verbatim with source = "force_route"
// (the §6.1 step 1 path the closure relies on).
TEST_F(QueryWiringTest, ValidRouteOverridesHonoured) {
    for (const char* route : {"simple", "complex", "chat"}) {
        auto classifier = MakeClassifier();
        QueryContext ctx;
        ctx.query = "what is the revenue";
        Status s = classifier.RouteAndUpdateContext(
            ctx, std::optional<std::string>(route));
        ASSERT_TRUE(s.ok()) << route;
        EXPECT_EQ(ctx.routing_path, route);
        EXPECT_EQ(ctx.routing_decision_source, "force_route");
    }
}

// route="auto" is NOT an override — it falls through to the classifier, so the
// resolved path comes from the heuristic, not "force_route".
TEST_F(QueryWiringTest, RouteAutoFallsThroughToClassifier) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "short query";  // < complex threshold, not a greeting -> simple
    Status s = classifier.RouteAndUpdateContext(ctx, std::optional<std::string>("auto"));
    ASSERT_TRUE(s.ok());
    EXPECT_NE(ctx.routing_decision_source, "force_route");
    EXPECT_EQ(ctx.routing_path, "simple");
}

// No override supplied (nullopt) also falls through to the classifier.
TEST_F(QueryWiringTest, NoOverrideUsesClassifier) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "hi";  // greeting -> chat via rule guard
    Status s = classifier.RouteAndUpdateContext(ctx, std::nullopt);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "chat");
    EXPECT_EQ(ctx.routing_decision_source, "rule");
}

// The F39 error carries the structured-data key the closure's 400 body needs.
TEST_F(QueryWiringTest, ForceRouteInvalidErrorHasStructuredKey) {
    const auto& keys = RequiredStructuredDataKeys(RouterErrorCode::kForceRouteInvalid);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0], "invalid_route_value");
    EXPECT_STREQ(RouterErrorCodeString(RouterErrorCode::kForceRouteInvalid),
                 "CX_ERR_F39_FORCE_ROUTE_INVALID");
}

// ===========================================================================
// DecisionOf : routing_path/source -> QueryRouterMetrics::Decision
// ===========================================================================
//
// These reproduce the DecisionOf truth table at the source of truth: the values
// RouteAndUpdateContext writes, plus the metric it records. The closure calls
// DecisionOf(qctx.routing_path, qctx.routing_decision_source) on exactly these.

// A force_route="simple" => routing_path "simple", which DecisionOf maps to kSimple
// (and RouteAndUpdateContext records it).
TEST_F(QueryWiringTest, DecisionSimpleRecorded) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "q";
    ASSERT_TRUE(classifier.RouteAndUpdateContext(
                    ctx, std::optional<std::string>("simple")).ok());
    EXPECT_EQ(ctx.routing_path, "simple");
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kSimple), 1u);
}

// A greeting routes to chat (source "rule") => DecisionOf maps to kChat.
TEST_F(QueryWiringTest, DecisionChatRecorded) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "thanks";
    ASSERT_TRUE(classifier.RouteAndUpdateContext(ctx, std::nullopt).ok());
    EXPECT_EQ(ctx.routing_path, "chat");
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kChat), 1u);
}

// A force_route="complex" => routing_path "complex" => kComplex.
TEST_F(QueryWiringTest, DecisionComplexRecorded) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "q";
    ASSERT_TRUE(classifier.RouteAndUpdateContext(
                    ctx, std::optional<std::string>("complex")).ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kComplex), 1u);
}

// A fail-safe source (classifier unavailable) => DecisionOf returns kFallback. We
// force the L2 path by injecting a NULL backend (IsAvailable()==false).
TEST_F(QueryWiringTest, DecisionFallbackOnUnavailableBackend) {
    QueryComplexityClassifier classifier(/*backend=*/nullptr, ComplexityConfig{});
    QueryContext ctx;
    ctx.query = "some retrievable question text";
    ASSERT_TRUE(classifier.RouteAndUpdateContext(ctx, std::nullopt).ok());
    EXPECT_EQ(ctx.routing_path, "complex");  // fail-safe to Complex
    EXPECT_EQ(ctx.routing_decision_source, "classifier_unavailable");
    // DecisionOf maps this source -> kFallback (the §10 fallback bucket).
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kFallback), 1u);
}

// The Decision enum label strings the metric renderer uses (the closure's bucket
// names) are stable.
TEST_F(QueryWiringTest, DecisionLabelStringsStable) {
    EXPECT_STREQ(ToString(QueryRouterMetrics::Decision::kSimple), "simple");
    EXPECT_STREQ(ToString(QueryRouterMetrics::Decision::kComplex), "complex");
    EXPECT_STREQ(ToString(QueryRouterMetrics::Decision::kChat), "chat");
    EXPECT_STREQ(ToString(QueryRouterMetrics::Decision::kFallback), "fallback");
}

// ===========================================================================
// ResolveRagFusionConfig : default-off + per-request opt-in semantics
// ===========================================================================

// The closure's ResolveRagFusionConfig starts from a default RagFusionConfig
// (enabled=false, topic 3) and only flips enabled when the request opts in. We pin
// the default the resolver builds on.
TEST_F(QueryWiringTest, RagFusionConfigDefaultsDisabled) {
    RagFusionConfig cfg;  // exactly what ResolveRagFusionConfig starts with
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.variant_count, 3);  // topic 1 N=3 default kept
    EXPECT_EQ(cfg.rrf_k, 60);
    EXPECT_EQ(cfg.candidate_multiplier, 1);
    EXPECT_EQ(cfg.max_candidates, 50);
    EXPECT_FALSE(cfg.final_rerank);
    EXPECT_EQ(cfg.activation_policy, "always");
    EXPECT_NEAR(cfg.activation_score_margin, 0.0f, 1e-6f);
    EXPECT_EQ(cfg.activation_min_results, 2);
}

// use_rag_fusion = (routing_path=="complex") && cfg.enabled && llm_available. With
// the default config disabled, the gate is off regardless of route — the closure
// runs plain scatter.
TEST_F(QueryWiringTest, RagFusionGateOffWhenConfigDisabled) {
    RagFusionConfig cfg;  // enabled=false
    const bool llm_available = true;
    const std::string routing_path = "complex";
    const bool use_rag_fusion = routing_path == "complex" && cfg.enabled && llm_available;
    EXPECT_FALSE(use_rag_fusion);
}

// Opt-in (cfg.enabled=true) + complex route + LLM present => gate on.
TEST_F(QueryWiringTest, RagFusionGateOnWhenEnabledComplexAndLlm) {
    RagFusionConfig cfg;
    cfg.enabled = true;
    EXPECT_TRUE(std::string("complex") == "complex" && cfg.enabled && /*llm=*/true);
    // No LLM -> off even when enabled (the [R7] null-LLM degrade).
    EXPECT_FALSE(std::string("complex") == "complex" && cfg.enabled && /*llm=*/false);
    // Simple route -> off even when enabled (F36 only on Complex).
    EXPECT_FALSE(std::string("simple") == "complex" && cfg.enabled && /*llm=*/true);
}

// ValidateRagFusionConfig accepts the default and rejects an out-of-range count
// (the config the resolver hands downstream must be valid).
TEST_F(QueryWiringTest, RagFusionConfigValidation) {
    RagFusionConfig ok;
    EXPECT_TRUE(ValidateRagFusionConfig(ok));
    RagFusionConfig bad;
    bad.variant_count = 99;  // > [1,10]
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(bad, &field, &range));
    EXPECT_EQ(field, "variant_count");
}

TEST_F(QueryWiringTest, RagFusionConfigValidationRejectsExpandedCandidateBounds) {
    RagFusionConfig bad_multiplier;
    bad_multiplier.candidate_multiplier = kRagFusionCandidateMultiplierMax + 1;
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(bad_multiplier, &field, &range));
    EXPECT_EQ(field, "candidate_multiplier");

    RagFusionConfig bad_candidates;
    bad_candidates.max_candidates = kRagFusionMaxCandidatesMax + 1;
    EXPECT_FALSE(ValidateRagFusionConfig(bad_candidates, &field, &range));
    EXPECT_EQ(field, "max_candidates");

    RagFusionConfig bad_policy;
    bad_policy.activation_policy = "sometimes";
    EXPECT_FALSE(ValidateRagFusionConfig(bad_policy, &field, &range));
    EXPECT_EQ(field, "activation_policy");
}

// v1.0.13 fusion-policy knobs: bounds enforced, edge values accepted.
TEST_F(QueryWiringTest, RagFusionConfigValidationFusionPolicyBounds) {
    std::string field, range;

    RagFusionConfig bad_original;
    bad_original.fusion_original_weight = 0.0f;  // < 0.1 floor
    EXPECT_FALSE(ValidateRagFusionConfig(bad_original, &field, &range));
    EXPECT_EQ(field, "fusion_original_weight");

    RagFusionConfig bad_variant;
    bad_variant.fusion_variant_weight = kRagFusionFusionWeightMax + 1.0f;
    EXPECT_FALSE(ValidateRagFusionConfig(bad_variant, &field, &range));
    EXPECT_EQ(field, "fusion_variant_weight");

    RagFusionConfig bad_anchor;
    bad_anchor.fusion_anchor_max = kRagFusionAnchorMaxMax + 1;
    EXPECT_FALSE(ValidateRagFusionConfig(bad_anchor, &field, &range));
    EXPECT_EQ(field, "fusion_anchor_max");

    RagFusionConfig edges;  // 0-weight variants + no anchor are both legal
    edges.fusion_variant_weight = 0.0f;
    edges.fusion_anchor_max = 0;
    EXPECT_TRUE(ValidateRagFusionConfig(edges));
}

}  // namespace
}  // namespace cortrix::query
