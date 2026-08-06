#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/query/query_context.h"
#include "cortrix/query/query_context_explain.h"
#include "cortrix/query/crag_stage.h"
#include "cortrix/query/cross_ns_response.h"
#include "cortrix/retrieval/crag_classifier_backend.h"
#include "cortrix/retrieval/crag_config.h"
#include "cortrix/retrieval/crag_error.h"  // CragInferenceError (Throwing mock)
#include "cortrix/retrieval/crag_evaluator.h"
#include "cortrix/retrieval/crag_metrics.h"
#include "cortrix/retrieval/routing_mock.h"
#include "cortrix/retrieval/heuristic_guard_backend.h"
#include "cortrix/retrieval/types.h"

// CRAG S4/S5/S7 coverage: EvaluateAndUpdateContext writes the 6 CRAG QueryContext
// fields + path handling, the explain-endpoint dump, and the query routing
// ShouldSkipCrag mock. Standalone — heuristic backend, no ONNX.
namespace cortrix::retrieval {
namespace {

RankedChunk Chunk(const std::string& id, float score) {
    return RankedChunk{id, "t", "p", score, 0.0f, {}, {}};
}

query::CrossNsResponse ResponseWithScores(std::initializer_list<float> scores) {
    query::CrossNsResponse resp;
    int idx = 0;
    for (float score : scores) {
        ResultItem item;
        item.child_id = "child-" + std::to_string(idx++);
        item.content = "t";
        item.parent_content = "p";
        item.score = score;
        item.rerank_score = score;
        resp.results.push_back(std::move(item));
    }
    return resp;
}

class CragContextTest : public ::testing::Test {
protected:
    void SetUp() override { CragMetrics::Instance().ResetForTest(); }
    void TearDown() override { CragMetrics::Instance().ResetForTest(); }
};

// ---------------- EvaluateAndUpdateContext: field writes + paths ----------

TEST_F(CragContextTest, CorrectVerdictWritesFieldsNoAction) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    query::QueryContext ctx;
    ctx.query = "a real query";
    ctx.ns_id = "ns-1";
    ev.EvaluateAndUpdateContext(ctx, {Chunk("a", 0.95f), Chunk("b", 0.9f)});

    EXPECT_EQ(ctx.crag_verdict, "correct");
    EXPECT_GE(ctx.crag_score, 0.7f);
    EXPECT_FALSE(ctx.crag_signals.empty());
    EXPECT_NE(ctx.crag_signals.find("top1"), ctx.crag_signals.end());
    // Correct path: no ambiguous action, web fallback stays false.
    EXPECT_EQ(ctx.ambiguous_action_taken, "");
    EXPECT_FALSE(ctx.web_fallback_triggered);
    // Metric incremented for `correct`.
    EXPECT_EQ(CragMetrics::Instance().EvaluationCount(CragMetrics::Decision::kCorrect), 1u);
}

TEST_F(CragContextTest, AmbiguousVerdictSetsAction) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    query::QueryContext ctx;
    ctx.query = "a real query";
    // top1=0.5, ratio=0.5 → 0.5 → ambiguous.
    ev.EvaluateAndUpdateContext(ctx, {Chunk("a", 0.5f), Chunk("b", 0.2f)});

    EXPECT_EQ(ctx.crag_verdict, "ambiguous");
    EXPECT_EQ(ctx.ambiguous_action_taken, "filtered_top_n_by_2");
    EXPECT_EQ(CragMetrics::Instance().EvaluationCount(CragMetrics::Decision::kAmbiguous), 1u);
}

TEST_F(CragContextTest, IncorrectVerdictRecordsAndKeepsWebFallbackFalse) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    query::QueryContext ctx;
    ctx.query = "a real query";
    ev.EvaluateAndUpdateContext(ctx, {Chunk("a", 0.1f), Chunk("b", 0.05f)});

    EXPECT_EQ(ctx.crag_verdict, "incorrect");
    EXPECT_FALSE(ctx.web_fallback_triggered);  // Phase 1 always false
    EXPECT_EQ(ctx.ambiguous_action_taken, "");
    EXPECT_EQ(CragMetrics::Instance().EvaluationCount(CragMetrics::Decision::kIncorrect), 1u);
}

TEST_F(CragContextTest, DegradeVerdictCountsAsFallbackAndTakesCorrectPath) {
    // No backend → not available → heuristic. To force the degrade verdict we use a
    // throwing backend via the evaluator's L3 path.
    class Throwing : public ICragClassifierBackend {
    public:
        CragBackendResult Infer(const std::string&, const std::string&,
                                const std::map<std::string, float>&) override {
            throw CragInferenceError("x");
        }
        std::string Name() const override { return "throwing"; }
        bool IsAvailable() const override { return true; }
    };
    CragConfig cfg;
    cfg.max_inference_retries = 0;
    CragEvaluator ev(std::make_shared<Throwing>(), cfg);
    query::QueryContext ctx;
    ctx.query = "a real query";
    ev.EvaluateAndUpdateContext(ctx, {Chunk("a", 0.9f)});

    EXPECT_EQ(ctx.crag_verdict, "correct_fallback_classifier_failed");
    // Folds into the `fallback` metric bucket; takes the correct (no-op) path.
    EXPECT_EQ(CragMetrics::Instance().EvaluationCount(CragMetrics::Decision::kFallback), 1u);
    EXPECT_EQ(ctx.ambiguous_action_taken, "");
}

TEST_F(CragContextTest, DoesNotTouchRouterFields) {
    // write-failure isolation: CRAG only writes its own fields, never query routing's.
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    query::QueryContext ctx;
    ctx.query = "a real query";
    ctx.routing_path = "complex";
    ctx.complexity_score = 0.83f;
    ctx.routing_decision_source = "llm";
    ev.EvaluateAndUpdateContext(ctx, {Chunk("a", 0.95f)});

    // Query routing fields untouched.
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_FLOAT_EQ(ctx.complexity_score, 0.83f);
    EXPECT_EQ(ctx.routing_decision_source, "llm");
    // Cross-NS query execution fields also untouched (pure-ADD unification).
    EXPECT_EQ(ctx.top_k, 10);
    EXPECT_TRUE(ctx.rerank);
}

// ---------------- ShouldSkipCrag mock --------------------------------

TEST_F(CragContextTest, ShouldSkipCragMockByRoutingPath) {
    query::QueryContext complex;
    complex.routing_path = "complex";
    EXPECT_FALSE(ShouldSkipCrag(complex));  // complex → run CRAG

    query::QueryContext simple;
    simple.routing_path = "simple";
    EXPECT_TRUE(ShouldSkipCrag(simple));    // simple → skip

    query::QueryContext chat;
    chat.routing_path = "chat";
    EXPECT_TRUE(ShouldSkipCrag(chat));

    query::QueryContext chatFlag;
    chatFlag.chat_path_triggered = true;
    EXPECT_TRUE(ShouldSkipCrag(chatFlag));

    query::QueryContext unrouted;  // empty routing_path → default to running (fail-safe)
    EXPECT_FALSE(ShouldSkipCrag(unrouted));
}

TEST_F(CragContextTest, CragStageDisabledDoesNotEvaluateOrTruncate) {
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    query::CragStage stage(&ev);
    query::QueryContext ctx;
    ctx.query = "a real query";
    ctx.routing_path = "complex";
    ctx.enable_crag = false;
    query::CrossNsResponse resp = ResponseWithScores({0.5f, 0.2f, 0.1f});

    stage.Apply(resp, ctx);

    EXPECT_EQ(resp.results.size(), 3u);
    EXPECT_EQ(ctx.crag_verdict, "");
    EXPECT_EQ(ctx.ambiguous_action_taken, "");
    EXPECT_EQ(CragMetrics::Instance().EvaluationCount(CragMetrics::Decision::kAmbiguous), 0u);
}

// ---------------- explain-endpoint dump ----------------------------------

TEST_F(CragContextTest, ExplainJsonContainsAll13FieldsSnakeCase) {
    query::QueryContext ctx;
    ctx.query = "q";
    ctx.ns_id = "ns-1";
    ctx.routing_path = "complex";
    ctx.complexity_score = 0.78f;
    ctx.routing_decision_source = "llm";
    ctx.crag_verdict = "correct";
    ctx.crag_score = 0.92f;
    ctx.crag_signals = {{"top1", 0.95f}, {"median", 0.45f}};

    nlohmann::json j = query::ToExplainJson(ctx);

    // All 13 SPEC fields present (none omitted — SPEC).
    for (const char* key : {"query", "ns_id", "multi_turn_context_warning",
                            "web_fallback_triggered", "routing_path", "complexity_score",
                            "routing_decision_source", "chat_path_triggered", "crag_verdict",
                            "crag_score", "ambiguous_action_taken", "crag_signals",
                            "routing_misclassified"}) {
        EXPECT_TRUE(j.contains(key)) << "missing key: " << key;
    }
    EXPECT_EQ(j["crag_verdict"], "correct");
    EXPECT_NEAR(j["crag_score"].get<float>(), 0.92f, 1e-4);
    EXPECT_EQ(j["routing_path"], "complex");
    EXPECT_NEAR(j["crag_signals"]["top1"].get<float>(), 0.95f, 1e-4);
    // Default-valued fields are emitted, not omitted.
    EXPECT_EQ(j["ambiguous_action_taken"], "");
    EXPECT_EQ(j["web_fallback_triggered"], false);
}

TEST_F(CragContextTest, ExplainJsonRoundTripsAfterEvaluation) {
    // End-to-end: evaluate then dump → the verdict shows up in the explain node.
    CragEvaluator ev(std::make_shared<HeuristicGuardBackend>(), CragConfig{});
    query::QueryContext ctx;
    ctx.query = "a real query";
    ev.EvaluateAndUpdateContext(ctx, {Chunk("a", 0.5f), Chunk("b", 0.2f)});

    nlohmann::json j = query::ToExplainJson(ctx);
    EXPECT_EQ(j["crag_verdict"], "ambiguous");
    EXPECT_EQ(j["ambiguous_action_taken"], "filtered_top_n_by_2");
    EXPECT_TRUE(j["crag_signals"].contains("top1"));
}

}  // namespace
}  // namespace cortrix::retrieval
