#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "cortrix/query/complexity_classifier_backend.h"
#include "cortrix/query/complexity_config.h"
#include "cortrix/query/heuristic_complexity_backend.h"
#include "cortrix/query/query_complexity_classifier.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/query_router_metrics.h"
#include "cortrix/query/router_error.h"

// Query routing coverage: QueryComplexityClassifier — the routing algorithm (§6.1, all 7
// routing_decision_source values), L1/L2/L3 fallbacks (§7), confidence threshold
// (§6.1 step 5), the chat rule guard + multi-turn detection (§6.2), the
// IClassifier contract (S1), and the RAG-Fusion/CRAG skip helpers (§6.3). Standalone: the
// backend is a stub (HeuristicComplexityBackend or the programmable mock below).
namespace cortrix::query {
namespace {

using retrieval::ClassificationResult;
using retrieval::ClassifierInput;

// A programmable backend: returns a fixed label/confidence, or throws
// RouterInferenceError a configurable number of times before succeeding (to drive
// the §7.3 L3 retry/degrade path). Mirrors the CRAG mock-backend test pattern.
class MockComplexityBackend : public IComplexityClassifierBackend {
public:
    std::string label = "simple";
    float confidence = 0.9f;
    int throw_times = 0;        // number of leading Infer() calls that throw
    int infer_calls = 0;        // total Infer() invocations (observed by tests)
    bool available = true;

    ComplexityBackendResult Infer(const std::string& /*query*/) override {
        ++infer_calls;
        if (throw_times > 0) {
            --throw_times;
            throw RouterInferenceError("mock transient inference fault");
        }
        ComplexityBackendResult r;
        r.label = label;
        r.confidence = confidence;
        return r;
    }
    std::string Name() const override { return "mock_complexity"; }
    bool IsAvailable() const override { return available; }
};

ComplexityConfig DefaultConfig() { return ComplexityConfig{}; }

class RouterClassifierTest : public ::testing::Test {
protected:
    void SetUp() override { QueryRouterMetrics::Instance().ResetForTest(); }
    void TearDown() override { QueryRouterMetrics::Instance().ResetForTest(); }
};

// --- IClassifier contract (S1) -------------------------------------------------

TEST_F(RouterClassifierTest, IClassifierIdentity) {
    auto backend = std::make_shared<HeuristicComplexityBackend>();
    QueryComplexityClassifier c(backend, DefaultConfig());
    EXPECT_EQ(c.Name(), "routing-complexity");
    EXPECT_EQ(c.Labels(), (std::vector<std::string>{"simple", "complex", "chat"}));
    EXPECT_TRUE(c.IsAvailable());
}

TEST_F(RouterClassifierTest, IsAvailableFalseWhenBackendNull) {
    QueryComplexityClassifier c(nullptr, DefaultConfig());
    EXPECT_FALSE(c.IsAvailable());
}

TEST_F(RouterClassifierTest, IsAvailableFalseWhenBackendUnavailable) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->available = false;
    QueryComplexityClassifier c(backend, DefaultConfig());
    EXPECT_FALSE(c.IsAvailable());
}

TEST_F(RouterClassifierTest, ClassifyReturnsBackendLabelAndRecordsLatency) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "complex";
    backend->confidence = 0.8f;
    QueryComplexityClassifier c(backend, DefaultConfig());

    ClassifierInput in{"compare a vs b", {}, std::nullopt};
    ClassificationResult r = c.Classify(in);
    EXPECT_EQ(r.label, "complex");
    EXPECT_NEAR(r.confidence, 0.8f, 1e-6);
    EXPECT_FALSE(r.error_code.has_value());
    // latency observed once
    EXPECT_EQ(QueryRouterMetrics::Instance().ClassifierLatencyCount(), 1u);
}

TEST_F(RouterClassifierTest, ClassifyDegradesToComplexWhenUnavailable) {
    QueryComplexityClassifier c(nullptr, DefaultConfig());
    ClassifierInput in{"anything", {}, std::nullopt};
    ClassificationResult r = c.Classify(in);
    EXPECT_EQ(r.label, "complex");  // L2 safe fallback
}

// --- §6.1 step 1: Agent ?route override ---------------------------------------

TEST_F(RouterClassifierTest, ForceRouteOverridesToSimple) {
    auto backend = std::make_shared<MockComplexityBackend>();
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "a long detailed multi part question about many things and more";
    Status s = c.RouteAndUpdateContext(ctx, std::string("simple"));
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "simple");
    EXPECT_FLOAT_EQ(ctx.complexity_score, 1.0f);
    EXPECT_EQ(ctx.routing_decision_source, "force_route");
    EXPECT_FALSE(ctx.chat_path_triggered);
    EXPECT_EQ(backend->infer_calls, 0);  // override short-circuits the classifier
}

TEST_F(RouterClassifierTest, ForceRouteChatSetsChatFlag) {
    auto backend = std::make_shared<MockComplexityBackend>();
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "what is the revenue";
    Status s = c.RouteAndUpdateContext(ctx, std::string("chat"));
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "chat");
    EXPECT_TRUE(ctx.chat_path_triggered);
    EXPECT_EQ(ctx.routing_decision_source, "force_route");
}

TEST_F(RouterClassifierTest, ForceRouteAutoFallsThroughToClassifier) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "simple";
    backend->confidence = 0.9f;
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx, std::string("auto"));
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_decision_source, "llm");  // classifier ran
    EXPECT_EQ(ctx.routing_path, "simple");
    EXPECT_EQ(backend->infer_calls, 1);
}

TEST_F(RouterClassifierTest, ForceRouteInvalidReturnsErrorAndLeavesCtxUntouched) {
    auto backend = std::make_shared<MockComplexityBackend>();
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "hello world";
    Status s = c.RouteAndUpdateContext(ctx, std::string("banana"));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_ROUTER_FORCE_ROUTE_INVALID"), std::string::npos);
    EXPECT_NE(s.message().find("banana"), std::string::npos);
    // ctx untouched (routing_path still default empty).
    EXPECT_EQ(ctx.routing_path, "");
    EXPECT_EQ(ctx.routing_decision_source, "");
}

// --- §6.1 step 2: NS-config force_route ---------------------------------------

TEST_F(RouterClassifierTest, NsForceRouteOverrides) {
    auto backend = std::make_shared<MockComplexityBackend>();
    ComplexityConfig cfg = DefaultConfig();
    cfg.force_route = "complex";
    QueryComplexityClassifier c(backend, cfg);
    QueryContext ctx;
    ctx.query = "short";
    Status s = c.RouteAndUpdateContext(ctx);  // no Agent override
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_EQ(ctx.routing_decision_source, "ns_force_route");
    EXPECT_FLOAT_EQ(ctx.complexity_score, 1.0f);
    EXPECT_EQ(backend->infer_calls, 0);
}

TEST_F(RouterClassifierTest, NsForceRouteInvalidReturnsError) {
    auto backend = std::make_shared<MockComplexityBackend>();
    ComplexityConfig cfg = DefaultConfig();
    cfg.force_route = "nonsense";
    QueryComplexityClassifier c(backend, cfg);
    QueryContext ctx;
    ctx.query = "x";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_ROUTER_FORCE_ROUTE_INVALID"), std::string::npos);
}

TEST_F(RouterClassifierTest, AgentOverrideBeatsNsForceRoute) {
    auto backend = std::make_shared<MockComplexityBackend>();
    ComplexityConfig cfg = DefaultConfig();
    cfg.force_route = "complex";  // NS says complex
    QueryComplexityClassifier c(backend, cfg);
    QueryContext ctx;
    ctx.query = "x";
    Status s = c.RouteAndUpdateContext(ctx, std::string("simple"));  // Agent says simple
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "simple");
    EXPECT_EQ(ctx.routing_decision_source, "force_route");
}

// --- §6.1 step 3: heuristic chat rule guard -----------------------------------

TEST_F(RouterClassifierTest, ChatRuleGuardRoutesGreeting) {
    auto backend = std::make_shared<MockComplexityBackend>();
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "Hello!";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "chat");
    EXPECT_TRUE(ctx.chat_path_triggered);
    EXPECT_EQ(ctx.routing_decision_source, "rule");
    EXPECT_FLOAT_EQ(ctx.complexity_score, 1.0f);
    EXPECT_EQ(backend->infer_calls, 0);  // rule guard short-circuits
}

TEST_F(RouterClassifierTest, IsChatQueryRecognizesPleasantries) {
    EXPECT_TRUE(QueryComplexityClassifier::IsChatQuery("hi"));
    EXPECT_TRUE(QueryComplexityClassifier::IsChatQuery("Hello"));
    EXPECT_TRUE(QueryComplexityClassifier::IsChatQuery("Thanks!"));
    EXPECT_TRUE(QueryComplexityClassifier::IsChatQuery("thank you"));
    EXPECT_TRUE(QueryComplexityClassifier::IsChatQuery("  bye  "));
    EXPECT_TRUE(QueryComplexityClassifier::IsChatQuery("\xe4\xbd\xa0\xe5\xa5\xbd"));  // hello
}

TEST_F(RouterClassifierTest, IsChatQueryRejectsContentQueries) {
    // §14 risk: a content question must NOT be mistaken for Chat (it would skip
    // all retrieval). "what is Cortrix" is a real retrieval query.
    EXPECT_FALSE(QueryComplexityClassifier::IsChatQuery("what is Cortrix"));
    EXPECT_FALSE(QueryComplexityClassifier::IsChatQuery("find the Q3 revenue report"));
    EXPECT_FALSE(QueryComplexityClassifier::IsChatQuery("hello world database schema"));
    EXPECT_FALSE(QueryComplexityClassifier::IsChatQuery(""));
}

// --- §7.1 L1: NS disabled / §7.2 L2: backend unavailable ----------------------

TEST_F(RouterClassifierTest, NsDisabledDefaultsToComplex) {
    auto backend = std::make_shared<MockComplexityBackend>();
    ComplexityConfig cfg = DefaultConfig();
    cfg.enabled = false;
    QueryComplexityClassifier c(backend, cfg);
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_EQ(ctx.routing_decision_source, "classifier_unavailable");
    EXPECT_EQ(backend->infer_calls, 0);
}

TEST_F(RouterClassifierTest, BackendUnavailableDefaultsToComplex) {
    QueryComplexityClassifier c(nullptr, DefaultConfig());
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_EQ(ctx.routing_decision_source, "classifier_unavailable");
}

// --- §6.1 step 4: main classifier path ----------------------------------------

TEST_F(RouterClassifierTest, ClassifierSimpleVerdictWritesLlmSource) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "simple";
    backend->confidence = 0.95f;
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "simple");
    EXPECT_NEAR(ctx.complexity_score, 0.95f, 1e-6);
    EXPECT_EQ(ctx.routing_decision_source, "llm");
    EXPECT_FALSE(ctx.chat_path_triggered);
}

TEST_F(RouterClassifierTest, ClassifierChatVerdictIsDemotedToComplex) {
    // D3.5 r2 S4: chat is double-gated. The chat path skips retrieval entirely,
    // and the Adaptive-RAG labels make the ML model misroute natural QA
    // sentences as chat (SQuAD2-unanswerable domain shift) - which would turn a
    // misroute into empty results. Only the rule guard may select chat; an ML
    // "chat" verdict demotes to the Complex fail-safe, observably.
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "chat";
    backend->confidence = 0.95f;
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    // a query that the rule guard does NOT catch but the classifier labels chat
    ctx.query = "tell me a joke please my friend";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_FALSE(ctx.chat_path_triggered);
    EXPECT_EQ(ctx.routing_decision_source, "chat_demoted_guard");
}

// --- §6.1 step 5: confidence threshold fail-safe ------------------------------

TEST_F(RouterClassifierTest, LowConfidenceFallsBackToComplex) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "simple";
    backend->confidence = 0.4f;  // below default threshold 0.5
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_NEAR(ctx.complexity_score, 0.4f, 1e-6);
    EXPECT_EQ(ctx.routing_decision_source, "low_confidence_fallback");
    // fallback bucket incremented
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kFallback),
              1u);
}

TEST_F(RouterClassifierTest, NsThresholdClampedToUpperBound) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "simple";
    backend->confidence = 0.75f;  // below a clamped-to-0.8 threshold → fallback
    ComplexityConfig cfg = DefaultConfig();
    cfg.confidence_threshold = 0.95f;  // out of [0.3,0.8] → clamped to 0.8
    QueryComplexityClassifier c(backend, cfg);
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_EQ(ctx.routing_decision_source, "low_confidence_fallback");
}

TEST_F(RouterClassifierTest, NsThresholdClampedToLowerBound) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "simple";
    backend->confidence = 0.35f;  // above a clamped-to-0.3 threshold → accepted
    ComplexityConfig cfg = DefaultConfig();
    cfg.confidence_threshold = 0.1f;  // out of [0.3,0.8] → clamped to 0.3
    QueryComplexityClassifier c(backend, cfg);
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "simple");  // 0.35 >= 0.30 → accepted
    EXPECT_EQ(ctx.routing_decision_source, "llm");
}

// --- §7.3 L3: transient inference failure → retry then degrade ----------------

TEST_F(RouterClassifierTest, TransientFaultRetriedThenSucceeds) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->throw_times = 2;       // fail twice, succeed on the 3rd (budget=3)
    backend->label = "simple";
    backend->confidence = 0.9f;
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "simple");
    EXPECT_EQ(ctx.routing_decision_source, "llm");
    EXPECT_EQ(backend->infer_calls, 3);  // 2 throws + 1 success
}

TEST_F(RouterClassifierTest, AllRetriesFailDegradeToComplex) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->throw_times = 99;  // always throws → exhaust the retry budget
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());  // degrade is a normal outcome
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_EQ(ctx.routing_decision_source, "inference_failed_fallback");
    EXPECT_NEAR(ctx.complexity_score, 0.5f, 1e-6);
    EXPECT_EQ(backend->infer_calls, 3);  // default max_inference_retries
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kFallback),
              1u);
}

TEST_F(RouterClassifierTest, ClassifyTotalOnPersistentFault) {
    // IClassifier::Classify must be total: a persistently-throwing backend yields
    // the degraded Complex result with the INFERENCE_FAILED error_code, never an
    // exception across the boundary.
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->throw_times = 99;
    QueryComplexityClassifier c(backend, DefaultConfig());
    ClassificationResult r = c.Classify(ClassifierInput{"q", {}, std::nullopt});
    EXPECT_EQ(r.label, "complex");
    ASSERT_TRUE(r.error_code.has_value());
    EXPECT_EQ(*r.error_code, "CX_ERR_ROUTER_INFERENCE_FAILED");
}

// --- §6.2: multi-turn signal detection (borrowed 6) ---------------------------

TEST_F(RouterClassifierTest, MultiTurnSignalSetsWarning) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "complex";
    backend->confidence = 0.9f;
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "and what about its pricing details for the business plan";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_TRUE(ctx.multi_turn_context_warning);
}

TEST_F(RouterClassifierTest, MultiTurnWarningDisabledByConfig) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "complex";
    backend->confidence = 0.9f;
    ComplexityConfig cfg = DefaultConfig();
    cfg.multi_turn_warning_enabled = false;
    QueryComplexityClassifier c(backend, cfg);
    QueryContext ctx;
    ctx.query = "and what about its pricing";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_FALSE(ctx.multi_turn_context_warning);
}

TEST_F(RouterClassifierTest, HasMultiTurnSignalDetectsCuesAndAvoidsFalsePositives) {
    EXPECT_TRUE(QueryComplexityClassifier::HasMultiTurnSignal("what is it used for"));
    EXPECT_TRUE(QueryComplexityClassifier::HasMultiTurnSignal("tell me more about that"));
    EXPECT_TRUE(QueryComplexityClassifier::HasMultiTurnSignal(
        "\xe7\xbb\xa7\xe7\xbb\xad\xe8\xaf\xb4"));  // (continue)
    // "item" contains "it" but must NOT trigger (whole-word matching).
    EXPECT_FALSE(QueryComplexityClassifier::HasMultiTurnSignal("list every item in stock"));
    EXPECT_FALSE(QueryComplexityClassifier::HasMultiTurnSignal("find the revenue report"));
}

// --- §6.3: RAG-Fusion / CRAG skip helpers (lockstep with routing_mock.h) ----------

TEST_F(RouterClassifierTest, ShouldSkipRagFusionAndF37PerPath) {
    QueryContext simple;
    simple.routing_path = "simple";
    EXPECT_TRUE(QueryComplexityClassifier::ShouldSkipF36(simple));
    EXPECT_TRUE(QueryComplexityClassifier::ShouldSkipF37(simple));

    QueryContext complex;
    complex.routing_path = "complex";
    EXPECT_FALSE(QueryComplexityClassifier::ShouldSkipF36(complex));
    EXPECT_FALSE(QueryComplexityClassifier::ShouldSkipF37(complex));

    QueryContext chat;
    chat.routing_path = "chat";
    chat.chat_path_triggered = true;
    EXPECT_TRUE(QueryComplexityClassifier::ShouldSkipF36(chat));
    EXPECT_TRUE(QueryComplexityClassifier::ShouldSkipF37(chat));

    // Not-yet-routed (empty) → fail-safe to NOT skip (run the full pipeline).
    QueryContext fresh;
    EXPECT_FALSE(QueryComplexityClassifier::ShouldSkipF36(fresh));
    EXPECT_FALSE(QueryComplexityClassifier::ShouldSkipF37(fresh));
}

TEST_F(RouterClassifierTest, ChatFlagAloneTriggersSkipEvenWithoutRoutingPath) {
    // Defensive: chat_path_triggered set but routing_path somehow empty → still skip.
    QueryContext ctx;
    ctx.chat_path_triggered = true;
    EXPECT_TRUE(QueryComplexityClassifier::ShouldSkipF36(ctx));
    EXPECT_TRUE(QueryComplexityClassifier::ShouldSkipF37(ctx));
}

// --- end-to-end with the real heuristic backend (no mock) ---------------------

TEST_F(RouterClassifierTest, HeuristicBackendRoutesComparisonToComplex) {
    auto backend = std::make_shared<HeuristicComplexityBackend>();
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "compare Cortrix vs Mem0 retrieval precision in production scenarios";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_EQ(ctx.routing_decision_source, "llm");
}

TEST_F(RouterClassifierTest, HeuristicBackendRoutesShortQueryToSimple) {
    auto backend = std::make_shared<HeuristicComplexityBackend>();
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "Cortrix documentation overview";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "simple");
    EXPECT_EQ(ctx.routing_decision_source, "llm");
}

// --- §10 metric bucketing: fallback is distinct from a confident complex --------

TEST_F(RouterClassifierTest, ConfidentComplexCountsAsComplexNotFallback) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "complex";
    backend->confidence = 0.95f;  // well above threshold → confident
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "compare the cloud and community editions in depth somehow";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    auto& m = QueryRouterMetrics::Instance();
    EXPECT_EQ(m.DecisionCount(QueryRouterMetrics::Decision::kComplex), 1u);
    EXPECT_EQ(m.DecisionCount(QueryRouterMetrics::Decision::kFallback), 0u);
}

TEST_F(RouterClassifierTest, ClassifierUnavailableCountsAsFallback) {
    QueryComplexityClassifier c(nullptr, DefaultConfig());
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    auto& m = QueryRouterMetrics::Instance();
    EXPECT_EQ(m.DecisionCount(QueryRouterMetrics::Decision::kFallback), 1u);
    EXPECT_EQ(m.DecisionCount(QueryRouterMetrics::Decision::kComplex), 0u);
}

TEST_F(RouterClassifierTest, SimpleVerdictCountsAsSimple) {
    auto backend = std::make_shared<MockComplexityBackend>();
    backend->label = "simple";
    backend->confidence = 0.9f;
    QueryComplexityClassifier c(backend, DefaultConfig());
    QueryContext ctx;
    ctx.query = "find the onboarding guide";
    Status s = c.RouteAndUpdateContext(ctx);
    EXPECT_TRUE(s.ok());
    auto& m = QueryRouterMetrics::Instance();
    EXPECT_EQ(m.DecisionCount(QueryRouterMetrics::Decision::kSimple), 1u);
    EXPECT_EQ(m.DecisionCount(QueryRouterMetrics::Decision::kFallback), 0u);
}

}  // namespace
}  // namespace cortrix::query
