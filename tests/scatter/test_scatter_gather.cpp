#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/common/executor_engine.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/scatter_gather.h"
#include "cortrix/query/scatter_guc.h"
#include "mock_permission_service.h"
#include "mock_reranker.h"
#include "mock_response_builder.h"
#include "mock_scatter_executor.h"

// S1.4 + S2.3 + S3.2-S3.5 + S4.2-S4.3 coverage: ScatterGather — single/multi-NS
// split, dual-layer timeout, partial success meta, gather + B-simplified dedup, 8-field
// meta, RRF-disabled warning, GUC ApplyConfig, plan-cap effective limit, auth.
namespace cortrix::query {
namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;
using reranker::MockReranker;
using retrieval::NamespaceQueryResult;

QueryRequest Req(std::vector<std::string> ns, int top_k = 10, bool rerank = true) {
    QueryRequest r;
    r.query = "q";
    r.namespaces = std::move(ns);
    r.top_k = top_k;
    r.rerank = rerank;
    return r;
}

AuthContext Auth() {
    AuthContext a;
    a.user_id = "u1";
    a.tenant_id = "t1";
    return a;
}

// Helper to build a ScatterGather with the standard mocks. The ExecutorEngine and
// MockReranker outlive the call.
struct Harness {
    MockIScatterExecutor executor;
    ExecutorEngine engine{2, 100};
    MockReranker reranker;
    MockPermissionService perm;

    explicit Harness(std::vector<std::string> authorized) : perm(std::move(authorized)) {}

    ScatterGather Make() { return ScatterGather(&executor, &engine, &reranker, &perm); }
};

// --- Single-NS direct path (Issue 1.6): 1 authorized NS does NOT fan out. ---
TEST(ScatterGatherTest, SingleNsDirectPath) {
    Harness h({"ns_a"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 3)));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, /*top_k=*/2), Auth());

    EXPECT_EQ(resp.meta.namespaces_queried, (std::vector<std::string>{"ns_a"}));
    EXPECT_EQ(resp.meta.namespaces_succeeded, (std::vector<std::string>{"ns_a"}));
    EXPECT_TRUE(resp.meta.namespaces_failed.empty());
    EXPECT_FLOAT_EQ(resp.meta.coverage_ratio, 1.0f);
    ASSERT_EQ(resp.results.size(), 2u);                 // truncated to top_k
    EXPECT_EQ(resp.results[0].namespace_id, "ns_a");
    EXPECT_FALSE(resp.error.has_value());
}

// --- Multi-NS fan-out: results merged, sorted by rerank_score across NS. ---
TEST(ScatterGatherTest, MultiNsGatherSortedByRerankScore) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithRerankScores("ns_a", {0.7f, 0.3f})));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithRerankScores("ns_b", {0.9f, 0.5f})));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, /*top_k=*/3), Auth());

    ASSERT_EQ(resp.results.size(), 3u);  // 4 candidates → top_k 3
    EXPECT_FLOAT_EQ(resp.results[0].rerank_score, 0.9f);  // ns_b
    EXPECT_FLOAT_EQ(resp.results[1].rerank_score, 0.7f);  // ns_a
    EXPECT_FLOAT_EQ(resp.results[2].rerank_score, 0.5f);  // ns_b
    EXPECT_EQ(resp.meta.namespaces_succeeded.size(), 2u);
    EXPECT_FLOAT_EQ(resp.meta.coverage_ratio, 1.0f);
}

// --- Partial success: one NS index-corrupt → meta.namespaces_failed + coverage<1. ---
TEST(ScatterGatherTest, PartialSuccessRecordsFailedNamespace) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 2)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::IndexCorrupt("ns_b")));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, 10), Auth());

    EXPECT_EQ(resp.meta.namespaces_succeeded, (std::vector<std::string>{"ns_a"}));
    ASSERT_EQ(resp.meta.namespaces_failed.size(), 1u);
    const auto& f = resp.meta.namespaces_failed[0];
    EXPECT_EQ(f.namespace_id, "ns_b");
    EXPECT_EQ(f.error_code, "CX_ERR_INDEX_CORRUPT");
    EXPECT_EQ(f.category, "permanent");
    EXPECT_FALSE(f.retryable);
    EXPECT_FALSE(f.retry_after_ms.has_value());          // non-retryable → no retry_after_ms
    EXPECT_FLOAT_EQ(resp.meta.coverage_ratio, 0.5f);     // 1 of 2 succeeded
    EXPECT_FALSE(resp.error.has_value());                // partial NS failure ≠ scatter error
}

// A failed NS that IS retryable (timeout) carries retry_after_ms (GEN-Agent #6).
TEST(ScatterGatherTest, TimedOutNamespaceCarriesRetryAfterMs) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 1)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Timeout("ns_b")));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, 10), Auth());
    ASSERT_EQ(resp.meta.namespaces_failed.size(), 1u);
    const auto& f = resp.meta.namespaces_failed[0];
    EXPECT_EQ(f.error_code, "CX_ERR_NS_TIMEOUT");
    EXPECT_EQ(f.category, "timeout");
    EXPECT_TRUE(f.retryable);
    ASSERT_TRUE(f.retry_after_ms.has_value());
    EXPECT_EQ(*f.retry_after_ms, 1000);
}

// --- Dual-layer timeout: a slow NS exceeding total_timeout_ms → CX_ERR_NS_TIMEOUT
//     in meta.failed AND top-level CX_ERR_SCATTER_TIMEOUT (partial 200). ---
TEST(ScatterGatherTest, OverallTimeoutYieldsScatterTimeoutPartial) {
    Harness h({"ns_fast", "ns_slow"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_fast", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_fast", 2)));
    // ns_slow blocks well past the (tiny) overall deadline.
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_slow", _))
        .WillOnce(Invoke([](const QueryContext&, const std::string& ns, float) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));

    auto sg = h.Make();
    ScatterTimeouts t;
    t.ns_query_timeout_ms = 1000;  // per-NS generous...
    t.total_timeout_ms = 50;       // ...but overall is tiny → scatter timeout
    sg.SetTimeouts(t);

    auto resp = sg.Execute(Req({"ns_fast", "ns_slow"}, 10), Auth());

    // ns_fast succeeded; ns_slow recorded as timed out.
    EXPECT_NE(std::find(resp.meta.namespaces_succeeded.begin(),
                        resp.meta.namespaces_succeeded.end(), "ns_fast"),
              resp.meta.namespaces_succeeded.end());
    ASSERT_FALSE(resp.meta.namespaces_failed.empty());
    EXPECT_EQ(resp.meta.namespaces_failed[0].namespace_id, "ns_slow");
    EXPECT_EQ(resp.meta.namespaces_failed[0].error_code, "CX_ERR_NS_TIMEOUT");
    // Top-level scatter timeout (partial 200, retryable).
    ASSERT_TRUE(resp.error.has_value());
    EXPECT_EQ(resp.error->code, "CX_ERR_SCATTER_TIMEOUT");
    EXPECT_TRUE(resp.error->retryable);
    EXPECT_EQ(resp.error->category, agent_friendly::ErrorCategory::kTimeout);
    EXPECT_FALSE(resp.results.empty());  // partial results from ns_fast preserved
}

// --- Auth integration: unauthorized NS throws (does not return a response). ---
TEST(ScatterGatherTest, UnauthorizedNamespaceThrows) {
    Harness h({"ns_a"});  // ns_secret NOT authorized
    auto sg = h.Make();
    try {
        sg.Execute(Req({"ns_a", "ns_secret"}, 10), Auth());
        FAIL() << "expected CrossNsException";
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kNsUnauthorized);
    }
}

// Unauthenticated → CX_ERR_AUTH_INVALID_CREDENTIALS (before any executor call).
TEST(ScatterGatherTest, UnauthenticatedThrows) {
    Harness h({"ns_a"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, _, _)).Times(0);
    auto sg = h.Make();
    AuthContext anon;
    EXPECT_THROW(sg.Execute(Req({"ns_a"}, 10), anon), CrossNsException);
}

// Wildcard ["*"] expands via the permission service and queries all authorized NS.
TEST(ScatterGatherTest, WildcardQueriesAllAuthorized) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 1)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_b", 1)));
    auto sg = h.Make();
    auto resp = sg.Execute(Req({"*"}, 10), Auth());
    EXPECT_EQ(resp.meta.namespaces_queried, (std::vector<std::string>{"ns_a", "ns_b"}));
    EXPECT_EQ(resp.meta.namespaces_succeeded.size(), 2u);
}

// Over the hard cap → CX_ERR_TOO_MANY_NAMESPACES.
TEST(ScatterGatherTest, OverHardCapThrows) {
    std::vector<std::string> many;
    for (int i = 0; i < 6; ++i) many.push_back("ns_" + std::to_string(i));
    Harness h(many);
    auto sg = h.Make();
    sg.SetMaxNamespaces(5);
    try {
        sg.Execute(Req(many, 10), Auth());
        FAIL() << "expected CrossNsException";
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kTooManyNamespaces);
    }
}

// RRF fallback (rerank=false) surfaces the meta.warnings status (Issue 3.2).
TEST(ScatterGatherTest, RerankDisabledEmitsWarning) {
    Harness h({"ns_a"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 1)));
    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, 10, /*rerank=*/false), Auth());
    ASSERT_EQ(resp.meta.warnings.size(), 1u);
    EXPECT_EQ(resp.meta.warnings[0]["reason"], "rerank_disabled");
}

// 8-field meta: every A/C-class field is present on a normal multi-NS response.
TEST(ScatterGatherTest, MetaHasAllEightFields) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 1)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_b", 1)));
    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, 10), Auth());

    auto j = resp.meta.ToJson();
    for (const char* key : {"namespaces_queried", "namespaces_succeeded", "namespaces_failed",
                            "coverage_ratio", "latency_ms", "deduplicated_chunks",
                            "deduplicated_chunks_count", "warnings"}) {
        EXPECT_TRUE(j.contains(key)) << "missing meta field: " << key;
    }
    EXPECT_EQ(j.size(), 8u);  // exactly 8 — no rerank_enabled / dedup_applied (B2 deleted)
}

// --- S3.3 in-gather: two NS returning identical chunk text collapse to one primary
//     and populate meta.deduplicated_chunks (content_hash is content-derived). ---
TEST(ScatterGatherTest, GatherDedupesCrossNsDuplicateContent) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithDuplicateContent("ns_a", "same body", 0.6f)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::WithDuplicateContent("ns_b", "same body", 0.9f)));
    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, 10), Auth());

    ASSERT_EQ(resp.results.size(), 1u);              // collapsed
    EXPECT_EQ(resp.results[0].namespace_id, "ns_b"); // higher rerank_score = primary
    EXPECT_EQ(resp.meta.deduplicated_chunks_count, 1);
    ASSERT_EQ(resp.meta.deduplicated_chunks.size(), 1u);
    EXPECT_EQ(resp.meta.deduplicated_chunks[0].primary_namespace, "ns_b");
    EXPECT_EQ(resp.meta.deduplicated_chunks[0].namespaces.size(), 2u);
}

// Distinct content across NS → no dedup, both kept, deduplicated_chunks empty.
TEST(ScatterGatherTest, GatherKeepsDistinctContent) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 1)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_b", 1)));
    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, 10), Auth());
    EXPECT_EQ(resp.results.size(), 2u);
    EXPECT_EQ(resp.meta.deduplicated_chunks_count, 0);
    EXPECT_TRUE(resp.meta.deduplicated_chunks.empty());
}

// --- S4.2: ApplyConfig sets the global NS cap + dual-layer timeouts from a GUC-
//     resolved ScatterConfig. ---
TEST(ScatterGatherTest, ApplyConfigSetsCapAndTimeouts) {
    Harness h({"ns_a"});
    auto sg = h.Make();
    ScatterConfig cfg;
    cfg.max_namespaces_per_query = 7;
    cfg.ns_query_timeout_ms = 1500;
    cfg.total_timeout_ms = 9000;
    sg.ApplyConfig(cfg);
    EXPECT_EQ(sg.max_namespaces(), 7);
    EXPECT_EQ(sg.timeouts().ns_query_timeout_ms, 1500);
    EXPECT_EQ(sg.timeouts().total_timeout_ms, 9000);
}

// --- S4.3: the effective cap is min(global cap, plan cap). The frozen AuthContext
//     has no plan field today, so the cap == the global cap and a request at the
//     cap passes while one over it is rejected. ---
TEST(ScatterGatherTest, GlobalCapEnforcedAtEffectiveLimit) {
    std::vector<std::string> many;
    for (int i = 0; i < 4; ++i) many.push_back("ns_" + std::to_string(i));
    Harness h(many);
    auto sg = h.Make();
    sg.SetMaxNamespaces(3);  // global cap 3; AuthContext has no lower plan cap
    try {
        sg.Execute(Req(many, 10), Auth());  // 4 > 3
        FAIL() << "expected CrossNsException";
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kTooManyNamespaces);
    }
}

}  // namespace
}  // namespace cortrix::query
