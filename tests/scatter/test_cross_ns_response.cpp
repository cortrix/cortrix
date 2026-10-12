#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/cross_ns_response.h"
#include "cortrix/retrieval/cross_ns_types.h"

// S2.5 + S3.1 surface coverage: CrossNsResponse / CrossNsMeta JSON schema
// compliance (§2.5) — ResultItem uses child_id:string (V5 decision #6A, NOT block_id:int)
// + the GEN-Agent Agent-friendly response shape.
namespace cortrix::query {
namespace {

retrieval::ResultItem MakeItem(const std::string& child, const std::string& ns, float rr) {
    retrieval::ResultItem it;
    it.child_id = child;
    it.parent_id = "parent_" + child;
    it.namespace_id = ns;
    it.content = "content of " + child;
    it.parent_content = "parent content";
    it.score = 0.7f;
    it.rerank_score = rr;
    it.content_hash = "sha256:00000000000000000000000000000000";
    it.metadata = {{"source", "doc.pdf"}};
    return it;
}

// ResultItem serializes with child_id/parent_id as STRINGS (ULID), not an int
// block_id (§2.5 v1.0.3 — JS number-precision safe).
TEST(CrossNsResponseTest, ResultItemUsesStringChildId) {
    CrossNsResponse resp;
    resp.results.push_back(MakeItem("01HXYZ", "ns_a", 0.9f));
    auto j = resp.ToJson();

    ASSERT_TRUE(j.contains("results"));
    ASSERT_EQ(j["results"].size(), 1u);
    const auto& item = j["results"][0];
    EXPECT_TRUE(item["child_id"].is_string());
    EXPECT_EQ(item["child_id"], "01HXYZ");
    EXPECT_TRUE(item["parent_id"].is_string());
    EXPECT_FALSE(item.contains("block_id"));  // §2.5: block_id:int was DELETED
    // §2.5 field set
    for (const char* key : {"child_id", "parent_id", "content", "parent_content", "score",
                            "rerank_score", "namespace", "content_hash", "metadata"}) {
        EXPECT_TRUE(item.contains(key)) << "missing result field: " << key;
    }
    EXPECT_EQ(item["namespace"], "ns_a");
    EXPECT_EQ(item["content_hash"], "sha256:00000000000000000000000000000000");
    EXPECT_FALSE(item.contains("score_signals"));
}

TEST(CrossNsResponseTest, ResultItemSerializesScoreSignalsWhenPresent) {
    CrossNsResponse resp;
    auto item = MakeItem("01HSIGNAL", "ns_a", 0.9f);
    item.score_signals.enriched_score = 0.8f;
    item.score_signals.semantic_score = 1.0f;
    resp.results.push_back(item);

    auto j = resp.ToJson();
    const auto& signals = j["results"][0]["score_signals"];
    ASSERT_TRUE(signals.is_object());
    EXPECT_FLOAT_EQ(signals["enriched_score"].get<float>(), 0.8f);
    EXPECT_FLOAT_EQ(signals["semantic_score"].get<float>(), 1.0f);
}

// meta serializes exactly the 8 A/C-class fields (§2.5 / Issue 3.5 v1.0.2).
TEST(CrossNsResponseTest, MetaSerializesExactlyEightFields) {
    CrossNsMeta meta;
    meta.namespaces_queried = {"ns_a", "ns_b"};
    meta.namespaces_succeeded = {"ns_a"};
    meta.coverage_ratio = 0.5f;
    meta.latency_ms = 123;

    auto j = meta.ToJson();
    EXPECT_EQ(j.size(), 8u);
    EXPECT_FALSE(j.contains("rerank_enabled"));  // B2 deleted
    EXPECT_FALSE(j.contains("dedup_applied"));    // B2 deleted
    EXPECT_EQ(j["coverage_ratio"], 0.5);
    EXPECT_EQ(j["latency_ms"], 123);
    EXPECT_TRUE(j["namespaces_failed"].is_array());
    EXPECT_TRUE(j["deduplicated_chunks"].is_array());
    EXPECT_EQ(j["deduplicated_chunks_count"], 0);
}

// A namespaces_failed[] entry carries the full GEN-Agent 4 field (§2.5 / 2.7).
TEST(CrossNsResponseTest, FailedNamespaceEntryIsAgentFriendly) {
    CrossNsMeta meta;
    NamespaceFailure f;
    f.namespace_id = "ns_b";
    f.error_code = "CX_ERR_NS_TIMEOUT";
    f.retryable = true;
    f.category = "timeout";
    f.retry_after_ms = 1000;
    f.message = "ns_b timed out";
    meta.namespaces_failed.push_back(f);

    auto j = meta.ToJson();
    ASSERT_EQ(j["namespaces_failed"].size(), 1u);
    const auto& fj = j["namespaces_failed"][0];
    EXPECT_EQ(fj["namespace"], "ns_b");
    EXPECT_EQ(fj["error_code"], "CX_ERR_NS_TIMEOUT");
    EXPECT_EQ(fj["retryable"], true);
    EXPECT_EQ(fj["category"], "timeout");
    EXPECT_EQ(fj["retry_after_ms"], 1000);
    EXPECT_EQ(fj["message"], "ns_b timed out");
}

// R10 GAP-E4: every namespaces_failed[] entry must carry the structured_data its
// code requires (GEN-Agent #5 / cross_ns_error.h RequiredStructuredDataKeys). The
// serializer always injects "namespace"; the producer supplies code-specific keys
// (kIndexCorrupt → index_state). Pre-fix the entry had no structured_data at all,
// violating the response's own schema contract.
TEST(CrossNsResponseTest, FailedEntryCarriesRequiredStructuredData) {
    CrossNsMeta meta;
    NamespaceFailure f;
    f.namespace_id = "ns_b";
    f.error_code = "CX_ERR_INDEX_CORRUPT";
    f.retryable = false;
    f.category = "permanent";
    f.structured_data = {{"index_state", "unavailable"}};  // producer-supplied key
    meta.namespaces_failed.push_back(f);

    auto j = meta.ToJson();
    const auto& sd = j["namespaces_failed"][0]["structured_data"];
    ASSERT_TRUE(sd.is_object());
    EXPECT_EQ(sd["namespace"], "ns_b");       // injected by serializer
    EXPECT_EQ(sd["index_state"], "unavailable");  // from producer
    // The serialized body satisfies the code's required-key contract.
    EXPECT_TRUE(HasRequiredStructuredData(CrossNsErrorCode::kIndexCorrupt, sd));
}

// kNsTimeout requires only {namespace}; the serializer injects it even when the
// producer set no structured_data, so the contract still holds.
TEST(CrossNsResponseTest, TimeoutFailureGetsNamespaceStructuredData) {
    CrossNsMeta meta;
    NamespaceFailure f;
    f.namespace_id = "ns_c";
    f.error_code = "CX_ERR_NS_TIMEOUT";
    f.retryable = true;
    f.category = "timeout";
    f.retry_after_ms = 1000;
    meta.namespaces_failed.push_back(f);  // no producer structured_data

    auto j = meta.ToJson();
    const auto& sd = j["namespaces_failed"][0]["structured_data"];
    EXPECT_EQ(sd["namespace"], "ns_c");
    EXPECT_TRUE(HasRequiredStructuredData(CrossNsErrorCode::kNsTimeout, sd));
}

// A non-retryable failure serializes retry_after_ms as null (GEN-Agent #6).
TEST(CrossNsResponseTest, NonRetryableFailureHasNullRetryAfterMs) {
    CrossNsMeta meta;
    NamespaceFailure f;
    f.namespace_id = "ns_b";
    f.error_code = "CX_ERR_INDEX_CORRUPT";
    f.retryable = false;
    f.category = "permanent";
    // retry_after_ms left unset
    meta.namespaces_failed.push_back(f);
    auto j = meta.ToJson();
    EXPECT_TRUE(j["namespaces_failed"][0]["retry_after_ms"].is_null());
}

// deduplicated_chunks[] entry shape (§2.5 / §3.3 B-simplified multi-source).
TEST(CrossNsResponseTest, DeduplicatedChunkEntryShape) {
    CrossNsMeta meta;
    DeduplicatedChunkInfo d;
    d.content_hash = "sha256:abc";
    d.primary_namespace = "ns_a";
    d.namespaces = {{"ns_a", 0.92f, 0.91f}, {"ns_b", 0.85f, 0.84f}};
    meta.deduplicated_chunks.push_back(d);
    meta.deduplicated_chunks_count = 1;

    auto j = meta.ToJson();
    ASSERT_EQ(j["deduplicated_chunks"].size(), 1u);
    const auto& dj = j["deduplicated_chunks"][0];
    EXPECT_EQ(dj["content_hash"], "sha256:abc");
    EXPECT_EQ(dj["primary_namespace"], "ns_a");
    ASSERT_EQ(dj["namespaces"].size(), 2u);
    EXPECT_EQ(dj["namespaces"][0]["namespace"], "ns_a");
    EXPECT_FLOAT_EQ(dj["namespaces"][0]["score"].get<float>(), 0.92f);
    EXPECT_FLOAT_EQ(dj["namespaces"][0]["rerank_score"].get<float>(), 0.91f);
    EXPECT_EQ(j["deduplicated_chunks_count"], 1);
}

// The scatter-timeout partial response serializes a top-level error block alongside
// results + meta (§2.5 — HTTP 200 + partial + error).
TEST(CrossNsResponseTest, ScatterTimeoutSerializesErrorBlock) {
    CrossNsResponse resp;
    resp.results.push_back(MakeItem("c0", "ns_a", 0.9f));
    resp.error = MakeCrossNsError(CrossNsErrorCode::kScatterTimeout, nlohmann::json::object(),
                                  "partial");
    auto j = resp.ToJson();
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], "CX_ERR_SCATTER_TIMEOUT");
    EXPECT_EQ(j["error"]["retryable"], true);
    EXPECT_EQ(j["error"]["category"], "timeout");
    EXPECT_FALSE(j["results"].empty());  // partial results still present
}

// ToResultItem maps RankedChunk → ResultItem (RETRIEVAL_TYPES_SPEC §1-bis).
TEST(CrossNsResponseTest, ToResultItemMapsRankedChunk) {
    retrieval::RankedChunk rc;
    rc.child_id = "c1";
    rc.parent_id = "p1";
    rc.chunk_text = "hello";
    rc.parent_text = "parent";
    rc.score = 0.4f;
    rc.rerank_score = 0.88f;
    rc.score_signals.enriched_score = 0.75f;
    rc.metadata = {{"k", "v"}};

    auto it = retrieval::ToResultItem(rc, "ns_x", "sha256:deadbeef");
    EXPECT_EQ(it.child_id, "c1");
    EXPECT_EQ(it.namespace_id, "ns_x");
    EXPECT_EQ(it.content, "hello");
    EXPECT_EQ(it.parent_content, "parent");
    EXPECT_FLOAT_EQ(it.rerank_score, 0.88f);
    ASSERT_TRUE(it.score_signals.enriched_score.has_value());
    EXPECT_FLOAT_EQ(*it.score_signals.enriched_score, 0.75f);
    EXPECT_EQ(it.content_hash, "sha256:deadbeef");
    EXPECT_EQ(it.metadata.at("k"), "v");
    // SPEC §1 v1.2: parent_id is sourced from the hit. Before the F34 link was
    // threaded through the executor this field could only ever be the default, so
    // the V5 #6A primary identifier was unusable.
    EXPECT_EQ(it.parent_id, "p1");
}

// evidence_kind rides through the conversion verbatim: the executor decides it at the
// point where chunk_text is assigned, which is the only place that knows what went in.
TEST(CrossNsResponseTest, ToResultItemPassesEvidenceKindThrough) {
    for (const char* kind : {"chunk", "document_summary", "document_label"}) {
        retrieval::RankedChunk rc;
        rc.child_id = "c1";
        rc.chunk_text = "whatever";
        rc.evidence_kind = kind;
        auto it = retrieval::ToResultItem(rc, "ns_x", "sha256:beef");
        EXPECT_EQ(it.evidence_kind, kind);
    }
}

// evidence_kind is A-class: always serialized, never conditional on the caller
// remembering to ask. A caller that cannot tell a passage from a document label is
// exactly the problem this field exists to fix.
TEST(CrossNsResponseTest, EvidenceKindIsAlwaysSerialized) {
    CrossNsResponse resp;
    retrieval::ResultItem item;
    item.child_id = "01HXYZ";
    item.content = "a generated summary";
    item.evidence_kind = "document_summary";
    resp.results.push_back(std::move(item));

    auto j = resp.ToJson();
    ASSERT_EQ(j["results"].size(), 1u);
    ASSERT_TRUE(j["results"][0].contains("evidence_kind"));
    EXPECT_EQ(j["results"][0]["evidence_kind"], "document_summary");
}

// document_coverage must NOT appear in meta: it is a B-class diagnostic, and meta is
// the fixed set of 8 A/C-class fields. This pins the decision so a later change has
// to argue with a failing test rather than quietly growing meta.
TEST(CrossNsResponseTest, DocumentCoverageIsNotInMeta) {
    CrossNsMeta meta;
    meta.document_coverage.push_back({"spec.md", 2, 7});

    auto j = meta.ToJson();
    EXPECT_EQ(j.size(), 8u);
    EXPECT_FALSE(j.contains("document_coverage"));
}

// An unknown denominator serializes as null, never as 0 — "0 of 0" would read as
// full coverage, which is the opposite of what an unknown total means.
TEST(CrossNsResponseTest, UnknownCoverageTotalSerializesAsNull) {
    std::vector<DocumentCoverageInfo> coverage;
    coverage.push_back({"known.md", 2, 7});
    coverage.push_back({"unknown.md", 1, 0});

    auto j = DocumentCoverageArrayToJson(coverage);
    ASSERT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0]["source_path"], "known.md");
    EXPECT_EQ(j[0]["returned_child_blocks"], 2);
    EXPECT_EQ(j[0]["total_child_blocks"], 7);
    EXPECT_EQ(j[1]["returned_child_blocks"], 1);
    EXPECT_TRUE(j[1]["total_child_blocks"].is_null());
}

// A hit with no owning parent (doc-level pseudo-result, or a legacy non-child row)
// still maps cleanly: parent_id stays empty rather than becoming a dangling id.
TEST(CrossNsResponseTest, ToResultItemLeavesParentIdEmptyWhenHitHasNoParent) {
    retrieval::RankedChunk rc;
    rc.child_id = "c2";
    rc.chunk_text = "doc summary";

    auto it = retrieval::ToResultItem(rc, "ns_y", "sha256:cafe");
    EXPECT_EQ(it.child_id, "c2");
    EXPECT_TRUE(it.parent_id.empty());
    EXPECT_TRUE(it.parent_content.empty());
}

// --- EnforceParentExpansionBudget ----------------------------------------------
//
// The budget has to bound the RESPONSE. A per-namespace pass honouring the same numbers
// independently is not a bound: with N namespaces each spending it, N times the budget
// reaches the caller. And the stages after the gather re-order and drop items, so a
// decision made before them describes a different list than the one sent.

namespace {

retrieval::ResultItem WithParent(const std::string& child_id, std::size_t parent_bytes) {
    retrieval::ResultItem it;
    it.child_id = child_id;
    it.content = "body of " + child_id;
    it.parent_content = std::string(parent_bytes, 'x');
    return it;
}

}  // namespace

// Two 80-byte parents arriving from two namespaces, each of which honoured a 100-byte
// budget on its own, must not leave 160 bytes on the wire.
TEST(EnforceParentExpansionBudgetTest, ByteCapBoundsTheResponseNotEachNamespace) {
    CrossNsResponse resp;
    resp.results.push_back(WithParent("c1", 80));
    resp.results.push_back(WithParent("c2", 80));

    EnforceParentExpansionBudget(resp, /*expand_top_k=*/2, /*max_bytes=*/100);

    EXPECT_EQ(resp.results[0].parent_content.size(), 80u);
    EXPECT_TRUE(resp.results[1].parent_content.empty())
        << "the second parent would push the response past the caller's byte budget";
}

// The depth bound applies to the final ordering, not to whatever each namespace ranked
// first before the gather re-sorted everything.
TEST(EnforceParentExpansionBudgetTest, DepthBoundAppliesToTheFinalOrdering) {
    CrossNsResponse resp;
    resp.results.push_back(WithParent("c1", 10));
    resp.results.push_back(WithParent("c2", 10));
    resp.results.push_back(WithParent("c3", 10));

    EnforceParentExpansionBudget(resp, /*expand_top_k=*/1, /*max_bytes=*/0);

    EXPECT_FALSE(resp.results[0].parent_content.empty());
    EXPECT_TRUE(resp.results[1].parent_content.empty());
    EXPECT_TRUE(resp.results[2].parent_content.empty());
}

// No depth requested keeps no parent text, including text a per-namespace pass already
// fetched.
TEST(EnforceParentExpansionBudgetTest, ZeroDepthClearsEverything) {
    CrossNsResponse resp;
    resp.results.push_back(WithParent("c1", 10));
    EnforceParentExpansionBudget(resp, /*expand_top_k=*/0, /*max_bytes=*/0);
    EXPECT_TRUE(resp.results[0].parent_content.empty());
}

// A cap below even the first parent keeps none, rather than emitting a partial span.
TEST(EnforceParentExpansionBudgetTest, CapBelowFirstParentKeepsNone) {
    CrossNsResponse resp;
    resp.results.push_back(WithParent("c1", 80));
    EnforceParentExpansionBudget(resp, /*expand_top_k=*/1, /*max_bytes=*/10);
    EXPECT_TRUE(resp.results[0].parent_content.empty());
}

// Called twice with the same limits, nothing further changes — the route applies it
// after the gather already did.
TEST(EnforceParentExpansionBudgetTest, IsIdempotent) {
    CrossNsResponse resp;
    resp.results.push_back(WithParent("c1", 40));
    resp.results.push_back(WithParent("c2", 40));

    EnforceParentExpansionBudget(resp, 2, 50);
    const std::string first = resp.results[0].parent_content;
    const bool second_cleared = resp.results[1].parent_content.empty();

    EnforceParentExpansionBudget(resp, 2, 50);
    EXPECT_EQ(resp.results[0].parent_content, first);
    EXPECT_EQ(resp.results[1].parent_content.empty(), second_cleared);
}

// An empty result set is a no-op, not a crash.
TEST(EnforceParentExpansionBudgetTest, EmptyResultSetIsANoOp) {
    CrossNsResponse resp;
    EXPECT_NO_THROW(EnforceParentExpansionBudget(resp, 4, 1024));
    EXPECT_TRUE(resp.results.empty());
}

}  // namespace
}  // namespace cortrix::query
