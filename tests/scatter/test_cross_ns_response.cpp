#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/cross_ns_response.h"
#include "cortrix/retrieval/cross_ns_types.h"

// S2.5 + S3.1 surface coverage: CrossNsResponse / CrossNsMeta JSON schema
// compliance — ResultItem uses child_id:string (V5 decision #6A, NOT block_id:int)
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
// block_id (v1.0.3 — JS number-precision safe).
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
    EXPECT_FALSE(item.contains("block_id"));  //: block_id:int was DELETED
    // field set
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

// meta serializes exactly the 8 A/C-class fields (/ Issue 3.5 v1.0.2).
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

// A namespaces_failed[] entry carries the full GEN-Agent 4 field.
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

// deduplicated_chunks[] entry shape (B-simplified multi-source).
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
// results + meta (HTTP 200 + partial + error).
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

// ToResultItem maps RankedChunk → ResultItem (the retrieval-types spec).
TEST(CrossNsResponseTest, ToResultItemMapsRankedChunk) {
    retrieval::RankedChunk rc;
    rc.child_id = "c1";
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
}

}  // namespace
}  // namespace cortrix::query
