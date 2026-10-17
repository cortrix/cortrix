#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cortrix/common/executor_engine.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/namespace_timeout.h"
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

NamespaceQueryResult OneChunk(const std::string& ns_id,
                              const std::string& suffix,
                              float final_score,
                              float rerank_score) {
    NamespaceQueryResult r;
    r.namespace_id = ns_id;
    r.latency_ms = 10;
    retrieval::RankedChunk c;
    c.child_id = ns_id + "_" + suffix;
    c.chunk_text = ns_id + " " + suffix;
    c.score = final_score;
    c.rerank_score = rerank_score;
    r.chunks.push_back(std::move(c));
    return r;
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

TEST(ScatterGatherTest, SearchConfigPropagatesToQueryContext) {
    Harness h({"ns_a"});
    QueryRequest req = Req({"ns_a"}, /*top_k=*/2, /*rerank=*/false);
    req.search_config.enable_vector = true;
    req.search_config.enable_bm25 = false;
    req.search_config.enable_sparse = false;

    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Invoke([](const QueryContext& ctx, const std::string& ns, float) {
            EXPECT_TRUE(ctx.enable_vector);
            EXPECT_FALSE(ctx.enable_bm25);
            EXPECT_FALSE(ctx.enable_sparse);
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));

    auto sg = h.Make();
    auto resp = sg.Execute(req, Auth());
    EXPECT_EQ(resp.meta.namespaces_succeeded, (std::vector<std::string>{"ns_a"}));
}

// --- Multi-NS fan-out: results merged, sorted by final score across NS. ---
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

TEST(ScatterGatherTest, MultiNsGatherSortedByFinalScoreOverRerankScore) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(OneChunk("ns_a", "low_rerank_high_final", 0.9f, 0.1f)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _))
        .WillOnce(Return(OneChunk("ns_b", "high_rerank_low_final", 0.5f, 0.99f)));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, /*top_k=*/2), Auth());

    ASSERT_EQ(resp.results.size(), 2u);
    EXPECT_EQ(resp.results[0].namespace_id, "ns_a");
    EXPECT_FLOAT_EQ(resp.results[0].score, 0.9f);
    EXPECT_FLOAT_EQ(resp.results[0].rerank_score, 0.1f);
    EXPECT_EQ(resp.results[1].namespace_id, "ns_b");
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

// --- meta.document_coverage[] --------------------------------------------------
//
// How much of each returned document the response actually carried. The numerator is
// counted HERE rather than in the per-NS executor because only the gather knows the
// final (deduped, truncated) set; the denominator rides up on the per-NS result
// because only the executor has a store handle.

namespace {

// One chunk-evidence hit belonging to `doc_id`.
retrieval::RankedChunk ChunkHit(const std::string& child_id, const std::string& doc_id,
                                const std::string& source_path, float score) {
    retrieval::RankedChunk c;
    c.child_id = child_id;
    c.chunk_text = "body of " + child_id;
    c.score = score;
    c.rerank_score = score;
    c.evidence_kind = "chunk";
    c.metadata["doc_id"] = doc_id;
    c.metadata["source_path"] = source_path;
    return c;
}

// A doc-route hit: selected the document but carries none of its text.
retrieval::RankedChunk DocHit(const std::string& doc_id, const std::string& source_path,
                              float score) {
    retrieval::RankedChunk c;
    c.child_id = doc_id;
    c.chunk_text = "a generated summary";
    c.score = score;
    c.rerank_score = score;
    c.evidence_kind = "document_summary";
    c.metadata["doc_id"] = doc_id;
    c.metadata["source_path"] = source_path;
    return c;
}

}  // namespace

// Two chunks of one document, out of seven it has → "2 of 7".
TEST(DocumentCoverageTest, CountsReturnedChunksAgainstTheDocumentTotal) {
    Harness h({"ns_a"});
    retrieval::NamespaceQueryResult r;
    r.namespace_id = "ns_a";
    r.chunks.push_back(ChunkHit("c1", "doc_1", "spec.md", 0.9f));
    r.chunks.push_back(ChunkHit("c2", "doc_1", "spec.md", 0.8f));
    r.doc_child_block_totals["doc_1"] = 7;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(r));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, /*top_k=*/10), Auth());

    ASSERT_EQ(resp.meta.document_coverage.size(), 1u);
    EXPECT_EQ(resp.meta.document_coverage[0].source_path, "spec.md");
    EXPECT_EQ(resp.meta.document_coverage[0].returned_child_blocks, 2);
    EXPECT_EQ(resp.meta.document_coverage[0].total_child_blocks, 7);
}

// A doc-route hit is NOT a piece of the document's text, so it must read "0 of N".
// Counting it as 1 would claim evidence that was never returned.
TEST(DocumentCoverageTest, DocRouteHitContributesZeroReturnedBlocks) {
    Harness h({"ns_a"});
    retrieval::NamespaceQueryResult r;
    r.namespace_id = "ns_a";
    r.chunks.push_back(DocHit("doc_2", "guide.md", 0.7f));
    r.doc_child_block_totals["doc_2"] = 12;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(r));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, /*top_k=*/10), Auth());

    ASSERT_EQ(resp.meta.document_coverage.size(), 1u);
    EXPECT_EQ(resp.meta.document_coverage[0].returned_child_blocks, 0);
    EXPECT_EQ(resp.meta.document_coverage[0].total_child_blocks, 12);
}

// Mixed evidence for the same document: only the chunk counts toward the numerator.
TEST(DocumentCoverageTest, MixedEvidenceCountsOnlyTheChunk) {
    Harness h({"ns_a"});
    retrieval::NamespaceQueryResult r;
    r.namespace_id = "ns_a";
    r.chunks.push_back(ChunkHit("c1", "doc_3", "mixed.md", 0.9f));
    r.chunks.push_back(DocHit("doc_3", "mixed.md", 0.5f));
    r.doc_child_block_totals["doc_3"] = 4;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(r));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, /*top_k=*/10), Auth());

    ASSERT_EQ(resp.meta.document_coverage.size(), 1u);
    EXPECT_EQ(resp.meta.document_coverage[0].returned_child_blocks, 1);
    EXPECT_EQ(resp.meta.document_coverage[0].total_child_blocks, 4);
}

// An unknown denominator stays 0 in the struct, which the serializer emits as null.
// It must never silently become a real number.
TEST(DocumentCoverageTest, UnknownTotalStaysZeroRatherThanGuessed) {
    Harness h({"ns_a"});
    retrieval::NamespaceQueryResult r;
    r.namespace_id = "ns_a";
    r.chunks.push_back(ChunkHit("c1", "doc_4", "nototal.md", 0.9f));
    // doc_child_block_totals deliberately left empty (store could not count).
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(r));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, /*top_k=*/10), Auth());

    ASSERT_EQ(resp.meta.document_coverage.size(), 1u);
    EXPECT_EQ(resp.meta.document_coverage[0].returned_child_blocks, 1);
    EXPECT_EQ(resp.meta.document_coverage[0].total_child_blocks, 0);
}

// The numerator reflects the TRUNCATED set: a chunk cut by top_k is not "returned".
TEST(DocumentCoverageTest, NumeratorFollowsTheTopKCut) {
    Harness h({"ns_a"});
    retrieval::NamespaceQueryResult r;
    r.namespace_id = "ns_a";
    r.chunks.push_back(ChunkHit("c1", "doc_5", "cut.md", 0.9f));
    r.chunks.push_back(ChunkHit("c2", "doc_5", "cut.md", 0.8f));
    r.chunks.push_back(ChunkHit("c3", "doc_5", "cut.md", 0.7f));
    r.doc_child_block_totals["doc_5"] = 9;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(r));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, /*top_k=*/2), Auth());

    ASSERT_EQ(resp.results.size(), 2u);
    ASSERT_EQ(resp.meta.document_coverage.size(), 1u);
    EXPECT_EQ(resp.meta.document_coverage[0].returned_child_blocks, 2);
}

// Several documents: one entry each, in first-appearance order, so the response is
// deterministic for a caller diffing two runs.
TEST(DocumentCoverageTest, OneEntryPerDocumentInFirstAppearanceOrder) {
    Harness h({"ns_a"});
    retrieval::NamespaceQueryResult r;
    r.namespace_id = "ns_a";
    r.chunks.push_back(ChunkHit("c1", "doc_hi", "high.md", 0.9f));
    r.chunks.push_back(ChunkHit("c2", "doc_lo", "low.md", 0.5f));
    r.chunks.push_back(ChunkHit("c3", "doc_hi", "high.md", 0.4f));
    r.doc_child_block_totals["doc_hi"] = 3;
    r.doc_child_block_totals["doc_lo"] = 6;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(r));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, /*top_k=*/10), Auth());

    ASSERT_EQ(resp.meta.document_coverage.size(), 2u);
    EXPECT_EQ(resp.meta.document_coverage[0].source_path, "high.md");
    EXPECT_EQ(resp.meta.document_coverage[0].returned_child_blocks, 2);
    EXPECT_EQ(resp.meta.document_coverage[1].source_path, "low.md");
    EXPECT_EQ(resp.meta.document_coverage[1].returned_child_blocks, 1);
}

// Two namespaces, two different documents that happen to share a caller-selected
// doc_id. Through the real gather: the denominators must not be unioned into one
// entry, and the one surviving a trim must keep its own namespace's total.
TEST(DocumentCoverageTest, SharedDocIdAcrossNamespacesStaysTwoDocuments) {
    Harness h({"ns_a", "ns_b"});
    retrieval::NamespaceQueryResult a;
    a.namespace_id = "ns_a";
    a.chunks.push_back(ChunkHit("c_a", "shared-doc-id", "a.md", 0.9f));
    a.doc_child_block_totals["shared-doc-id"] = 2;
    retrieval::NamespaceQueryResult b;
    b.namespace_id = "ns_b";
    b.chunks.push_back(ChunkHit("c_b", "shared-doc-id", "b.md", 0.8f));
    b.doc_child_block_totals["shared-doc-id"] = 10;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(a));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _)).WillOnce(Return(b));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, /*top_k=*/10), Auth());

    ASSERT_EQ(resp.results.size(), 2u);
    ASSERT_EQ(resp.meta.document_coverage.size(), 2u)
        << "one returned chunk from each must not read as a single 2-of-2";
    EXPECT_EQ(resp.meta.document_coverage[0].namespace_id, "ns_a");
    EXPECT_EQ(resp.meta.document_coverage[0].returned_child_blocks, 1);
    EXPECT_EQ(resp.meta.document_coverage[0].total_child_blocks, 2);
    EXPECT_EQ(resp.meta.document_coverage[1].namespace_id, "ns_b");
    EXPECT_EQ(resp.meta.document_coverage[1].returned_child_blocks, 1);
    EXPECT_EQ(resp.meta.document_coverage[1].total_child_blocks, 10);
}

// Same shape, but top_k keeps only the lower-scored namespace's hit: it must carry
// ITS denominator (10), not the other document's (2).
TEST(DocumentCoverageTest, SharedDocIdSurvivorKeepsItsOwnDenominator) {
    Harness h({"ns_a", "ns_b"});
    retrieval::NamespaceQueryResult a;
    a.namespace_id = "ns_a";
    a.chunks.push_back(ChunkHit("c_a", "shared-doc-id", "a.md", 0.4f));
    a.doc_child_block_totals["shared-doc-id"] = 2;
    retrieval::NamespaceQueryResult b;
    b.namespace_id = "ns_b";
    b.chunks.push_back(ChunkHit("c_b", "shared-doc-id", "b.md", 0.9f));
    b.doc_child_block_totals["shared-doc-id"] = 10;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(a));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_b", _)).WillOnce(Return(b));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, /*top_k=*/1), Auth());

    ASSERT_EQ(resp.results.size(), 1u);
    EXPECT_EQ(resp.results[0].namespace_id, "ns_b");
    ASSERT_EQ(resp.meta.document_coverage.size(), 1u);
    EXPECT_EQ(resp.meta.document_coverage[0].namespace_id, "ns_b");
    EXPECT_EQ(resp.meta.document_coverage[0].total_child_blocks, 10);
}

// A hit with no doc_id cannot be attributed, so it produces no entry rather than an
// entry keyed on an empty string.
TEST(DocumentCoverageTest, HitWithoutDocIdProducesNoEntry) {
    Harness h({"ns_a"});
    retrieval::NamespaceQueryResult r;
    r.namespace_id = "ns_a";
    retrieval::RankedChunk orphan;
    orphan.child_id = "orphan";
    orphan.chunk_text = "no document";
    orphan.score = 0.9f;
    orphan.rerank_score = 0.9f;
    orphan.evidence_kind = "chunk";
    r.chunks.push_back(std::move(orphan));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _)).WillOnce(Return(r));

    auto sg = h.Make();
    auto resp = sg.Execute(Req({"ns_a"}, /*top_k=*/10), Auth());

    ASSERT_EQ(resp.results.size(), 1u);
    EXPECT_TRUE(resp.meta.document_coverage.empty());
}


// A namespace that misses its deadline is abandoned by the join, but its task keeps
// running on the pool after Execute has returned. It must read its own copy of the
// context, never the caller's, which by then may be gone: the served route's context
// is a local of the request handler. Here the caller changes its context after the
// call returns, then lets the slow task go on; the task must still see the value the
// query was made with.
TEST(ScatterGatherTest, TimedOutTaskReadsItsOwnCopyOfTheContext) {
    Harness h({"ns_fast", "ns_slow"});
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    bool done = false;
    std::string seen_query;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_fast", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_fast", 1)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_slow", _))
        .WillOnce(Invoke([&](const QueryContext& ctx, const std::string& ns, float) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return release; });
            seen_query = ctx.query;  // read only after Execute has returned
            done = true;
            cv.notify_all();
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));
    auto sg = h.Make();
    ScatterTimeouts t;
    t.ns_query_timeout_ms = 1000;
    t.total_timeout_ms = 50;
    sg.SetTimeouts(t);

    QueryContext ctx;
    ctx.query = "the query as made";
    ctx.top_k = 10;
    auto resp = sg.Execute(Req({"ns_fast", "ns_slow"}, 10), Auth(), &ctx);
    ASSERT_TRUE(resp.error.has_value());
    ASSERT_EQ(resp.error->code, "CX_ERR_SCATTER_TIMEOUT")
        << "precondition: the join returned before the slow namespace finished";

    ctx.query = "changed by the caller after Execute returned";
    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    {
        std::unique_lock<std::mutex> lk(m);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; }));
    }
    EXPECT_EQ(seen_query, "the query as made");
}

// --- #173: the request's budget (QueryContext::deadline) ---------------------------

std::chrono::steady_clock::time_point In(int ms) {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

int MsSince(std::chrono::steady_clock::time_point t0) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
}

QueryContext BudgetCtx(std::chrono::steady_clock::time_point deadline) {
    QueryContext ctx;
    ctx.query = "q";
    ctx.top_k = 10;
    ctx.deadline = deadline;
    return ctx;
}

// The same as above with a budget and a lone namespace: it goes through the timed
// join, so its task is abandoned the same way and must read the same owned copy. The
// task waits for the release for a bounded time only, so a join that does not stop
// waiting fails the precondition instead of hanging.
TEST(ScatterGatherTest, TimedOutLoneNamespaceTaskReadsItsOwnCopyOfTheContext) {
    Harness h({"ns_slow"});
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    bool done = false;
    std::string seen_query;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_slow", _))
        .WillOnce(Invoke([&](const QueryContext& ctx, const std::string& ns, float) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::milliseconds(800), [&] { return release; });
            seen_query = ctx.query;
            done = true;
            cv.notify_all();
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));
    auto sg = h.Make();
    QueryContext ctx = BudgetCtx(In(100));
    ctx.query = "the query as made";
    auto resp = sg.Execute(Req({"ns_slow"}, 10), Auth(), &ctx);
    ASSERT_TRUE(resp.error.has_value())
        << "precondition: the join returned before the namespace finished";
    ASSERT_EQ(resp.error->code, "CX_ERR_SCATTER_TIMEOUT");

    ctx.query = "changed by the caller after Execute returned";
    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    {
        std::unique_lock<std::mutex> lk(m);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; }));
    }
    EXPECT_EQ(seen_query, "the query as made");
}

// A lone namespace with a budget is cut at the deadline: no results, its timeout row
// with the retry hint, and the top-level code. Its executor blocks for a bounded time,
// several times the budget, so a join that ignores the budget fails here, not hangs.
TEST(ScatterGatherTest, LoneNamespaceWithABudgetIsCutAtTheDeadline) {
    Harness h({"ns_slow"});
    std::atomic<bool> finished{false};
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_slow", _))
        .WillOnce(Invoke([&](const QueryContext&, const std::string& ns, float) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            finished = true;
            return ScatterMockResponseBuilder::Normal(ns, 2);
        }));
    auto sg = h.Make();
    QueryContext ctx = BudgetCtx(In(100));
    const auto t0 = std::chrono::steady_clock::now();
    auto resp = sg.Execute(Req({"ns_slow"}, 10), Auth(), &ctx);
    EXPECT_LT(MsSince(t0), 500) << "the join waited past the deadline";
    EXPECT_TRUE(resp.results.empty());
    ASSERT_EQ(resp.meta.namespaces_failed.size(), 1u);
    EXPECT_EQ(resp.meta.namespaces_failed[0].error_code, "CX_ERR_NS_TIMEOUT");
    ASSERT_TRUE(resp.meta.namespaces_failed[0].retry_after_ms.has_value());
    EXPECT_EQ(*resp.meta.namespaces_failed[0].retry_after_ms, 1000);
    ASSERT_TRUE(resp.error.has_value());
    EXPECT_EQ(resp.error->code, "CX_ERR_SCATTER_TIMEOUT");
    for (int i = 0; i < 200 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Guard (unchanged behaviour): without a budget a lone namespace still takes the
// direct path and is waited for, whatever the limits.
TEST(ScatterGatherTest, LoneNamespaceWithoutABudgetIsStillWaitedFor) {
    Harness h({"ns_a"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Invoke([](const QueryContext&, const std::string& ns, float) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));
    auto sg = h.Make();
    ScatterTimeouts t;
    t.ns_query_timeout_ms = 10;
    t.total_timeout_ms = 20;
    sg.SetTimeouts(t);
    auto resp = sg.Execute(Req({"ns_a"}, 10), Auth());
    EXPECT_EQ(resp.results.size(), 1u);
    EXPECT_TRUE(resp.meta.namespaces_failed.empty());
    EXPECT_FALSE(resp.error.has_value());
}

// Guard: a lone namespace is limited by the budget only. Slower than the per-namespace
// limit but within the budget, it succeeds.
TEST(ScatterGatherTest, LoneNamespaceIsLimitedByTheBudgetNotThePerNamespaceLimit) {
    Harness h({"ns_a"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Invoke([](const QueryContext&, const std::string& ns, float) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));
    auto sg = h.Make();
    ScatterTimeouts t;
    t.ns_query_timeout_ms = 50;
    t.total_timeout_ms = 30000;
    sg.SetTimeouts(t);
    QueryContext ctx = BudgetCtx(In(3000));
    auto resp = sg.Execute(Req({"ns_a"}, 10), Auth(), &ctx);
    EXPECT_EQ(resp.results.size(), 1u);
    EXPECT_TRUE(resp.meta.namespaces_failed.empty());
    EXPECT_FALSE(resp.error.has_value());
}

// A deadline that has already passed creates no task at all: every namespace gets
// its timeout row directly, and the response carries the top-level code.
TEST(ScatterGatherTest, PassedDeadlineSubmitsNoTask) {
    Harness h({"ns_a", "ns_b"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, _, _)).Times(0);
    auto sg = h.Make();
    QueryContext ctx = BudgetCtx(std::chrono::steady_clock::now() - std::chrono::milliseconds(1));
    auto resp = sg.Execute(Req({"ns_a", "ns_b"}, 10), Auth(), &ctx);
    EXPECT_TRUE(resp.results.empty());
    ASSERT_EQ(resp.meta.namespaces_failed.size(), 2u);
    for (const auto& f : resp.meta.namespaces_failed) {
        EXPECT_EQ(f.error_code, "CX_ERR_NS_TIMEOUT") << f.namespace_id;
    }
    ASSERT_TRUE(resp.error.has_value());
    EXPECT_EQ(resp.error->code, "CX_ERR_SCATTER_TIMEOUT");
}

// With several namespaces, a deadline shorter than the per-namespace limit cuts the
// slow one at the deadline; the fast one's results are kept.
TEST(ScatterGatherTest, DeadlineShorterThanThePerNamespaceLimitCutsAtTheDeadline) {
    Harness h({"ns_fast", "ns_slow"});
    std::atomic<bool> finished{false};
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_fast", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_fast", 1)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_slow", _))
        .WillOnce(Invoke([&](const QueryContext&, const std::string& ns, float) {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            finished = true;
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));
    auto sg = h.Make();
    ScatterTimeouts t;
    t.ns_query_timeout_ms = 5000;
    t.total_timeout_ms = 30000;
    sg.SetTimeouts(t);
    QueryContext ctx = BudgetCtx(In(100));
    const auto t0 = std::chrono::steady_clock::now();
    auto resp = sg.Execute(Req({"ns_fast", "ns_slow"}, 10), Auth(), &ctx);
    EXPECT_LT(MsSince(t0), 500) << "the join waited past the deadline";
    EXPECT_FALSE(resp.results.empty());
    ASSERT_EQ(resp.meta.namespaces_failed.size(), 1u);
    EXPECT_EQ(resp.meta.namespaces_failed[0].namespace_id, "ns_slow");
    EXPECT_EQ(resp.meta.namespaces_failed[0].error_code, "CX_ERR_NS_TIMEOUT");
    ASSERT_TRUE(resp.error.has_value());
    EXPECT_EQ(resp.error->code, "CX_ERR_SCATTER_TIMEOUT");
    for (int i = 0; i < 200 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Guard: a budget longer than the join's overall limit leaves the overall limit in
// charge.
TEST(ScatterGatherTest, BudgetLongerThanTheOverallLimitLeavesTheOverallLimitInCharge) {
    Harness h({"ns_fast", "ns_slow"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_fast", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_fast", 1)));
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_slow", _))
        .WillOnce(Invoke([](const QueryContext&, const std::string& ns, float) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));
    auto sg = h.Make();
    ScatterTimeouts t;
    t.ns_query_timeout_ms = 1000;
    t.total_timeout_ms = 50;
    sg.SetTimeouts(t);
    QueryContext ctx = BudgetCtx(In(5000));
    auto resp = sg.Execute(Req({"ns_fast", "ns_slow"}, 10), Auth(), &ctx);
    ASSERT_EQ(resp.meta.namespaces_failed.size(), 1u);
    EXPECT_EQ(resp.meta.namespaces_failed[0].namespace_id, "ns_slow");
    ASSERT_TRUE(resp.error.has_value());
    EXPECT_EQ(resp.error->code, "CX_ERR_SCATTER_TIMEOUT");
}

// A caller that passes no context gets the deadline from the request's timeout_ms,
// counted from the call.
TEST(ScatterGatherTest, BuiltContextTakesItsDeadlineFromTimeoutMs) {
    Harness h({"ns_a"});
    std::optional<std::chrono::steady_clock::time_point> seen;
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Invoke([&](const QueryContext& ctx, const std::string& ns, float) {
            seen = ctx.deadline;
            return ScatterMockResponseBuilder::Normal(ns, 1);
        }));
    auto sg = h.Make();
    QueryRequest r = Req({"ns_a"}, 10);
    r.timeout_ms = 2000;
    const auto before = std::chrono::steady_clock::now();
    auto resp = sg.Execute(r, Auth());
    const auto after = std::chrono::steady_clock::now();
    ASSERT_TRUE(seen.has_value());
    EXPECT_GE(*seen, before + std::chrono::milliseconds(2000));
    EXPECT_LE(*seen, after + std::chrono::milliseconds(2000));
}

TEST(NamespaceTimeoutTest, TimeoutRowCarriesTheErrorTableFields) {
    const auto row = MakeNamespaceTimeoutRow("ns_a", std::chrono::steady_clock::now());
    EXPECT_EQ(row.namespace_id, "ns_a");
    EXPECT_EQ(row.error_code, "CX_ERR_NS_TIMEOUT");
    EXPECT_EQ(row.error_category, "timeout");
    EXPECT_TRUE(row.retryable);
    EXPECT_EQ(row.retry_after_ms, 1000);
    EXPECT_TRUE(row.chunks.empty());
}

// The top-level code applies to a timeout row the join saw at or after the overall
// deadline, whoever wrote the row; never to a row that succeeded, and never before.
TEST(NamespaceTimeoutTest, CutByOverallDeadlineOnlyForATimeoutRowAtOrAfterIt) {
    const auto overall = std::chrono::steady_clock::now();
    const auto timed = MakeNamespaceTimeoutRow("ns", overall);
    EXPECT_TRUE(CutByOverallDeadline(timed, overall, overall));
    EXPECT_TRUE(CutByOverallDeadline(timed, overall + std::chrono::milliseconds(1), overall));
    EXPECT_FALSE(CutByOverallDeadline(timed, overall - std::chrono::milliseconds(1), overall));
    EXPECT_TRUE(CutByOverallDeadline(ScatterMockResponseBuilder::Timeout("ns"), overall, overall));
    EXPECT_FALSE(CutByOverallDeadline(ScatterMockResponseBuilder::Normal("ns", 1),
                                      overall + std::chrono::milliseconds(1), overall));
}

}  // namespace
}  // namespace cortrix::query
