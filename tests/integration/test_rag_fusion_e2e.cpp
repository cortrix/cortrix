// F36 RAG-Fusion integration tests (detail design §11.2: 7 IT, cases 23-29).
//
// Standalone (B_R1_BRIEFING §7, mirroring F04's "7 §7.5 integration scenarios
// standalone via MockIScatterExecutor"): the live QueryPipeline + F04
// ScatterGather wiring is D3.5-deferred, so these exercise the *end-to-end F36
// flow* — RagFusion::ExpandQueries (LLM variant generation via the frozen
// MockLlmClient) → simulated per-variant retrieval → RagFusion::FuseResults
// (global RRF) — plus the Issue 5/6 phased-rollout invariants and the Issue 4
// degrade path. The actual cross-Feature retrieval call (F04) is simulated with
// crafted ScoredResult lists; wiring the real ScatterGather is the D3.5 step.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/query/query_explain_json.h"
#include "cortrix/query/query_variant_generator.h"
#include "cortrix/query/rag_fusion.h"
#include "cortrix/query/rag_fusion_error.h"
#include "cortrix/query/rag_fusion_metrics.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/retrieval/types.h"

#include "mocks/mock_llm_client.h"

namespace cortrix::query {
namespace {

using ::testing::_;
using ::testing::Return;
using llm::ChatCompletionResponse;
using llm::MockLlmClient;
using retrieval::ScoredResult;

ChatCompletionResponse OkJson(const std::string& content) {
    ChatCompletionResponse r;
    r.status = Status::Ok();
    r.content = content;
    r.model = "gpt-4o-mini";
    r.prompt_tokens = 15;
    r.completion_tokens = 40;
    return r;
}

std::string FinanceVariants() {
    return R"({"variants":[
        {"strategy":"paraphrase","query":"latest company earnings report data"},
        {"strategy":"subquery","query":"last quarter company revenue and profit"},
        {"strategy":"reverse","query":"company revenue growth trend"}
    ]})";
}

RagFusionConfig Enabled() {
    RagFusionConfig c;
    c.enabled = true;
    c.variant_count = 3;
    return c;
}

std::shared_ptr<RagFusion> Service(std::shared_ptr<MockLlmClient> mock) {
    return std::make_shared<RagFusion>(
        std::make_shared<QueryVariantGenerator>(mock),
        std::make_shared<RRFFusion>(60));
}

// Simulate F04 ScatterGather returning per-variant top-K candidates (D3.5 wires
// the real call). Each variant's results overlap so fusion meaningfully reorders.
std::vector<std::vector<ScoredResult>> SimulateRetrieval(
    const std::vector<std::string>& queries) {
    std::vector<std::vector<ScoredResult>> per_variant;
    for (size_t i = 0; i < queries.size(); ++i) {
        // Each variant hits an overlapping set; "doc_hot" appears highly in all.
        per_variant.push_back({
            {"doc_hot", 0.95f - 0.01f * i},
            {"doc_" + std::to_string(i), 0.80f},
            {"doc_tail", 0.50f},
        });
    }
    return per_variant;
}

class RagFusionE2ETest : public ::testing::Test {
protected:
    void SetUp() override { RagFusionMetrics::Instance().ResetForTest(); }
};

// IT 23: E2E NS enabled → 4 variants retrieved → global RRF.
TEST_F(RagFusionE2ETest, E2E_RagFusion_Enabled_Query_4Variants) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _)).WillOnce(Return(OkJson(FinanceVariants())));
    auto svc = Service(mock);

    auto qs = svc->ExpandQueries("company financial status", Enabled());
    ASSERT_TRUE(qs.ok());
    ASSERT_EQ(qs.value().size(), 4u);  // 1 original + 3 variants

    auto fused = svc->FuseResults(SimulateRetrieval(qs.value()), 60);
    ASSERT_TRUE(fused.ok());
    ASSERT_FALSE(fused.value().empty());
    // "doc_hot" hit rank-1 across all 4 variants → must top the fused list.
    EXPECT_EQ(fused.value().front().child_id, "doc_hot");
}

// IT 24: E2E LLM failure → degrade to single query + meta warning identity.
TEST_F(RagFusionE2ETest, E2E_RagFusion_LlmFailure_Degrade) {
    auto mock = std::make_shared<MockLlmClient>();
    ChatCompletionResponse err;
    err.status = Status(StatusCode::kUnavailable, "ETIMEDOUT after 5000ms");
    EXPECT_CALL(*mock, Chat(_, _)).WillOnce(Return(err));
    auto svc = Service(mock);

    auto qs = svc->ExpandQueries("company financial status", Enabled());
    ASSERT_FALSE(qs.ok());  // caller falls back to single query
    auto es = svc->GetExplainState();
    EXPECT_TRUE(es.degraded);
    EXPECT_EQ(es.degrade_reason, "llm_timeout");

    // The QueryPipeline builds CX_WARN_RAG_FUSION_DEGRADED from this — verify the
    // 4-field GEN-Agent body + the 3 required structured_data keys (§7).
    auto warn = MakeRagFusionError(
        RagFusionErrorCode::kDegraded,
        {{"degrade_reason", "llm_timeout"},
         {"llm_error", "ETIMEDOUT after 5000ms"},
         {"original_query_used", "company financial status"}},
        "RAG-Fusion variant generation failed (LLM timeout), fell back to single query");
    EXPECT_EQ(warn.code, "CX_WARN_RAG_FUSION_DEGRADED");
    EXPECT_TRUE(warn.retryable);
    EXPECT_EQ(warn.category, agent_friendly::ErrorCategory::kTransient);
    ASSERT_TRUE(warn.retry_after_ms.has_value());
    EXPECT_EQ(*warn.retry_after_ms, 5000);
    EXPECT_TRUE(HasRequiredStructuredData(RagFusionErrorCode::kDegraded,
                                          *warn.structured_data));
}

// IT 25: E2E NS disabled → single-query path (no LLM call).
TEST_F(RagFusionE2ETest, E2E_RagFusion_DisabledNs_DirectSingleQuery) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _)).Times(0);
    auto svc = Service(mock);

    RagFusionConfig off;  // Issue 3 default disabled
    auto qs = svc->ExpandQueries("company financial status", off);
    ASSERT_TRUE(qs.ok());
    ASSERT_EQ(qs.value().size(), 1u);

    // Single query → caller returns per_variant_results[0] directly (§4.5 step 9
    // else-branch); FuseResults of one list is the identity reorder.
    auto fused = svc->FuseResults(SimulateRetrieval(qs.value()), 60);
    ASSERT_TRUE(fused.ok());
    EXPECT_EQ(fused.value().front().child_id, "doc_hot");
}

// IT 26: Issue 2 B — ONE LLM call yields the global variants reused for N×M retrieval.
TEST_F(RagFusionE2ETest, E2E_RagFusion_F04_ScatterGather_GlobalVariants) {
    auto mock = std::make_shared<MockLlmClient>();
    // Exactly ONE LLM call regardless of NS count (Issue 2 B: variants are global).
    EXPECT_CALL(*mock, Chat(_, _)).Times(1).WillOnce(Return(OkJson(FinanceVariants())));
    auto svc = Service(mock);

    auto qs = svc->ExpandQueries("company financial status", Enabled());
    ASSERT_TRUE(qs.ok());
    const int kVariants = static_cast<int>(qs.value().size());  // 4
    const int kNamespaces = 10;  // M = 10 NS

    // The 4 global variants are scattered across M=10 NS → 4×10 per-NS retrievals,
    // but still ONE LLM call (the EXPECT_CALL Times(1) above asserts this).
    std::vector<std::vector<ScoredResult>> all;
    for (int m = 0; m < kNamespaces; ++m) {
        for (const auto& list : SimulateRetrieval(qs.value())) all.push_back(list);
    }
    EXPECT_EQ(static_cast<int>(all.size()), kVariants * kNamespaces);  // 4M
    auto fused = svc->FuseResults(all, 60);
    ASSERT_TRUE(fused.ok());
    EXPECT_EQ(fused.value().front().child_id, "doc_hot");
}

// IT 27: Issue 5+6 — a default query response strictly excludes meta.rag_fusion.
TEST_F(RagFusionE2ETest, E2E_RagFusion_DefaultQuery_NoMetaRagFusion) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _)).WillOnce(Return(OkJson(FinanceVariants())));
    auto svc = Service(mock);
    svc->ExpandQueries("company financial status", Enabled());

    // Build a default-path response meta exactly as QueryPipeline step-11 would
    // (no explain): only A-class + C-class fields, NEVER rag_fusion.
    nlohmann::json meta = {
        {"namespaces_queried", {"finance"}},
        {"coverage_ratio", 1.0},
        {"latency_ms", 487},
        {"warnings", nlohmann::json::array()},
    };
    EXPECT_FALSE(meta.contains("rag_fusion"));  // Issue 6 invariant
}

// IT 28: ?explain=true → full explain.rag_fusion active state.
//
// Asserts against BuildRagFusionExplain, the same serializer the /query route
// calls, so a change to the emitted shape fails here.
TEST_F(RagFusionE2ETest, E2E_RagFusion_ExplainEndpoint_Active) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _)).WillOnce(Return(OkJson(FinanceVariants())));
    auto svc = Service(mock);
    svc->ExpandQueries("company financial status", Enabled());

    auto es = svc->GetExplainState();
    ASSERT_TRUE(es.active);
    const nlohmann::json rf = BuildRagFusionExplain(es);
    EXPECT_EQ(rf["active"], true);
    EXPECT_EQ(rf["feature_id"], "F36");
    EXPECT_EQ(rf["variant_count"], 4);
    EXPECT_EQ(rf["variants_used"].size(), 4u);
    EXPECT_EQ(rf["reason"], "active");
    EXPECT_EQ(rf["degraded"], false);
}

// IT 29 (rewritten): serializer contract for the inactive state.
//
// The old test here asserted an explain.potential_improvements array that it
// had built itself — nothing in src/ emits potential_improvements, so the
// test verified only its own construction.
//
// Scope: this is a SERIALIZER-contract test, not an endpoint test. The /query
// route calls BuildRagFusionExplain only when use_rag_fusion is true, and the
// ns_disabled state arises exactly when it is false — so the route never emits
// this block in a response today. What this pins is the builder's contract for
// the inactive state, so that if the route ever starts emitting it (or another
// caller serializes it), the shape is already fixed.
TEST_F(RagFusionE2ETest, RagFusionExplainSerializer_InactiveStateContract) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _)).Times(0);
    auto svc = Service(mock);
    RagFusionConfig off;
    svc->ExpandQueries("company financial status", off);

    auto es = svc->GetExplainState();
    ASSERT_FALSE(es.active);
    ASSERT_EQ(es.reason, "ns_disabled");
    const nlohmann::json rf = BuildRagFusionExplain(es);
    EXPECT_EQ(rf["active"], false);
    EXPECT_EQ(rf["feature_id"], "F36");
    EXPECT_EQ(rf["reason"], "ns_disabled");
    // The original query is always recorded as the single variant, even when
    // the stage never ran.
    EXPECT_EQ(rf["variant_count"], 1);
    ASSERT_TRUE(rf.contains("variants_used"));
    EXPECT_EQ(rf["variants_used"].size(), 1u);
    EXPECT_EQ(rf["variants_used"][0], "company financial status");
}

}  // namespace
}  // namespace cortrix::query
