#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "cortrix/common/executor_engine.h"
#include "cortrix/query/cross_ns_query_handler.h"
#include "cortrix/query/scatter_gather.h"
#include "cortrix/query/scatter_metrics.h"
#include "mock_permission_service.h"
#include "mock_reranker.h"
#include "mock_response_builder.h"
#include "mock_scatter_executor.h"

// S5.2 — the 7 §7.5 integration scenarios, driven standalone through the full
// CrossNsQueryHandler → ScatterGather pipeline over MockIScatterExecutor (§7.2):
//   IT-1 single-NS direct path / IT-2 3 NS / IT-3 10 NS (SLA) / IT-4 ["*"] wildcard /
//   IT-5 partial failure / IT-6 cross-NS same content_hash dedup / IT-7 rerank=false
//   RRF fallback. (Real per-NS pipeline / reranker / pgbench SLA = D3.5 / S5.3.)
namespace cortrix::query {
namespace {

using ::testing::_;
using ::testing::Return;
using reranker::MockReranker;

AuthContext Auth() {
    AuthContext a;
    a.user_id = "u1";
    a.tenant_id = "t1";
    return a;
}

// A full standalone stack: handler over a ScatterGather over the shared mocks.
struct Stack {
    MockIScatterExecutor executor;
    ExecutorEngine engine{4, 200};
    MockReranker reranker;
    MockPermissionService perm;
    ScatterGather scatter{&executor, &engine, &reranker, &perm};
    CrossNsQueryHandler handler{&scatter};

    explicit Stack(std::vector<std::string> authorized) : perm(std::move(authorized)) {}
};

nlohmann::json QueryBody(const std::vector<std::string>& ns, int top_k = 10,
                         bool rerank = true) {
    return nlohmann::json{{"query", "q"}, {"namespaces", ns}, {"top_k", top_k},
                          {"rerank", rerank}};
}

// === Case 1: single NS goes through cross-NS query direct path ===
TEST(ScatterIntegrationTest, IT1_SingleNsDirectPath) {
    Stack s({"ns_a"});
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 3)));

    HandlerResult r = s.handler.Handle(QueryBody({"ns_a"}, 3), Auth());
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["meta"]["namespaces_queried"].size(), 1u);
    EXPECT_EQ(r.body["meta"]["namespaces_succeeded"].size(), 1u);
    EXPECT_EQ(r.body["results"].size(), 3u);
    EXPECT_FLOAT_EQ(r.body["meta"]["coverage_ratio"].get<float>(), 1.0f);
}

// === Case 2: 3 NS Cross-NS query (merged + sorted by final score across NS) ===
TEST(ScatterIntegrationTest, IT2_ThreeNsCrossQuery) {
    Stack s({"ns_a", "ns_b", "ns_c"});
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithRerankScores("ns_a", {0.5f})));
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithRerankScores("ns_b", {0.9f})));
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_c", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithRerankScores("ns_c", {0.7f})));

    HandlerResult r = s.handler.Handle(QueryBody({"ns_a", "ns_b", "ns_c"}), Auth());
    EXPECT_EQ(r.status, 200);
    ASSERT_EQ(r.body["results"].size(), 3u);
    // cross-NS comparable ordering: 0.9 (ns_b) > 0.7 (ns_c) > 0.5 (ns_a).
    EXPECT_FLOAT_EQ(r.body["results"][0]["rerank_score"].get<float>(), 0.9f);
    EXPECT_EQ(r.body["results"][0]["namespace"], "ns_b");
    EXPECT_FLOAT_EQ(r.body["results"][2]["rerank_score"].get<float>(), 0.5f);
    EXPECT_EQ(r.body["meta"]["namespaces_succeeded"].size(), 3u);
}

// === Case 3: 10 NS Cross-NS (SLA tier — bucketed metric) ===
TEST(ScatterIntegrationTest, IT3_TenNsCrossQuery) {
    ScatterMetrics::Instance().ResetForTest();
    std::vector<std::string> ns;
    for (int i = 0; i < 10; ++i) ns.push_back("ns_" + std::to_string(i));
    Stack s(ns);
    for (const auto& n : ns) {
        EXPECT_CALL(s.executor, ExecuteForNamespace(_, n, _))
            .WillOnce(Return(ScatterMockResponseBuilder::Normal(n, 2)));
    }

    HandlerResult r = s.handler.Handle(QueryBody(ns, 5), Auth());
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["meta"]["namespaces_queried"].size(), 10u);
    EXPECT_EQ(r.body["meta"]["namespaces_succeeded"].size(), 10u);
    EXPECT_EQ(r.body["results"].size(), 5u);  // 20 candidates → top_k 5
    // duration histogram landed in the "10" bucket (≤10).
    EXPECT_EQ(ScatterMetrics::Instance().DurationCount(2), 1u);
    ScatterMetrics::Instance().ResetForTest();
}

// === Case 4: namespaces:["*"] wildcard (expands via the permission service) ===
TEST(ScatterIntegrationTest, IT4_WildcardExpansion) {
    Stack s({"ns_x", "ns_y"});
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_x", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_x", 1)));
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_y", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_y", 1)));

    HandlerResult r = s.handler.Handle(QueryBody({"*"}), Auth());
    EXPECT_EQ(r.status, 200);
    // ["*"] expanded to exactly the authorized set.
    EXPECT_EQ(r.body["meta"]["namespaces_queried"],
              (nlohmann::json{"ns_x", "ns_y"}));
}

// === Case 5: partial NS failure (meta.namespaces_failed with Agent friendly fields) ===
TEST(ScatterIntegrationTest, IT5_PartialFailure) {
    Stack s({"ns_ok", "ns_bad"});
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_ok", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_ok", 2)));
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_bad", _))
        .WillOnce(Return(ScatterMockResponseBuilder::IndexCorrupt("ns_bad")));

    HandlerResult r = s.handler.Handle(QueryBody({"ns_ok", "ns_bad"}), Auth());
    EXPECT_EQ(r.status, 200);  // partial success is still 200 (§2.7 principle 3)
    EXPECT_EQ(r.body["meta"]["namespaces_succeeded"], (nlohmann::json{"ns_ok"}));
    ASSERT_EQ(r.body["meta"]["namespaces_failed"].size(), 1u);
    const auto& f = r.body["meta"]["namespaces_failed"][0];
    EXPECT_EQ(f["namespace"], "ns_bad");
    EXPECT_EQ(f["error_code"], "CX_ERR_INDEX_CORRUPT");
    EXPECT_EQ(f["category"], "permanent");
    EXPECT_FALSE(f["retryable"].get<bool>());
    EXPECT_FLOAT_EQ(r.body["meta"]["coverage_ratio"].get<float>(), 0.5f);
}

// === Case 6: cross-NS same content_hash dedup (B-simplified) ===
TEST(ScatterIntegrationTest, IT6_CrossNsContentHashDedup) {
    Stack s({"ns_a", "ns_b"});
    // Both NS return a chunk with the SAME text → same content_hash → collapse.
    // ns_b has the higher rerank_score so it becomes the primary.
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithDuplicateContent(
            "ns_a", "shared chunk text", 0.70f)));
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithDuplicateContent(
            "ns_b", "shared chunk text", 0.95f)));

    HandlerResult r = s.handler.Handle(QueryBody({"ns_a", "ns_b"}), Auth());
    EXPECT_EQ(r.status, 200);
    // Collapsed to one result, primary = ns_b (highest final score).
    ASSERT_EQ(r.body["results"].size(), 1u);
    EXPECT_EQ(r.body["results"][0]["namespace"], "ns_b");
    // meta.deduplicated_chunks records the abbreviated multi-source (both NS + scores).
    EXPECT_EQ(r.body["meta"]["deduplicated_chunks_count"].get<int>(), 1);
    ASSERT_EQ(r.body["meta"]["deduplicated_chunks"].size(), 1u);
    const auto& d = r.body["meta"]["deduplicated_chunks"][0];
    EXPECT_EQ(d["primary_namespace"], "ns_b");
    EXPECT_EQ(d["namespaces"].size(), 2u);
}

// === Case 7: rerank=false RRF fallback (meta.warnings with "rerank_disabled") ===
TEST(ScatterIntegrationTest, IT7_RerankDisabledFallback) {
    Stack s({"ns_a"});
    EXPECT_CALL(s.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 2)));

    HandlerResult r = s.handler.Handle(QueryBody({"ns_a"}, 10, /*rerank=*/false), Auth());
    EXPECT_EQ(r.status, 200);
    ASSERT_EQ(r.body["meta"]["warnings"].size(), 1u);
    EXPECT_EQ(r.body["meta"]["warnings"][0]["reason"], "rerank_disabled");
}

}  // namespace
}  // namespace cortrix::query
