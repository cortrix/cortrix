// LiveSingleUnitExecutor — deterministic, no-model unit tests.
//
// Scope (honest): the live per-NS retrieval paths (chunk / doc / hybrid /
// sparse, child_id resolution, RRF+rerank assembly) require a live OnnxEmbedder
// (a real bge-m3 ONNX session) and a populated per-NS store, so they are NOT
// deterministic unit material — they belong to the docker E2E / live-model suite.
// What IS deterministic and dependency-free here:
//   1. CandidateK(top_k, oversample) — the over-fetch sizing arithmetic (mirrors
//      SingleUnitExecutor::CandidateK; the only pure method on this class), exercised
//      across the multiplier / max-cap / clamp-to-top_k / oversample-ceil branches.
//   2. FlattenMetadataIntoMap — the free function in this TU: the JSON-depth DoS
//      guard branch + non-object / invalid / collision branches not pinned by the
//      existing tests/scatter/test_live_metadata_flatten.cpp.
//
// The executor is constructed against a real (offline) F05 pool via NsPoolHarness
// and an un-Init()ed OnnxEmbedder: CandidateK touches neither the pool nor the
// embedder, so no model load or NS acquisition occurs.

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/json_depth.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/id/hash.h"
#include "cortrix/query/live_single_unit_executor.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store_sqlite.h"

#include "ns_pool_test_helper.h"  // cortrix::test::NsPoolHarness (real offline F05 pool)

namespace cortrix::query {
namespace {

// Build the deeply-nested object JSON string {"a":{"a":...{"a":1}...}} of `depth`.
std::string DeepObjectJson(int depth) {
    std::string s;
    s.reserve(static_cast<size_t>(depth) * 6 + 8);
    for (int i = 0; i < depth; ++i) s += "{\"a\":";
    s += "1";
    for (int i = 0; i < depth; ++i) s += "}";
    return s;
}

// Fixture: stands up the offline pool + a (never-Init()ed) embedder + RRF, and
// constructs the executor. CandidateK is pure arithmetic over the ctor-clamped
// multiplier/cap, so this exercises the real production object.
class LiveExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("live_exec_" + std::to_string(::testing::UnitTest::GetInstance()
                                                ->current_test_info()
                                                ->line())));
        // Un-Init()ed embedder: CandidateK never calls Embed(), so no model needed.
        embedder_ = std::make_unique<cortrix::OnnxEmbedder>("/nonexistent/model.onnx");
        rrf_ = std::make_unique<RRFFusion>(60);
    }

    std::unique_ptr<LiveSingleUnitExecutor> MakeExec(int multiplier, int max_cand) {
        return std::make_unique<LiveSingleUnitExecutor>(
            harness_->ipool(), *embedder_, *rrf_, /*reranker=*/nullptr,
            /*sparse_registry=*/nullptr, multiplier, max_cand);
    }

    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<cortrix::OnnxEmbedder> embedder_;
    std::unique_ptr<RRFFusion> rrf_;
};

// --- CandidateK: over-fetch sizing (multiplier × top_k, capped, clamped) ------

TEST_F(LiveExecutorTest, CandidateKBaseMultiplier) {
    auto exec = MakeExec(/*multiplier=*/3, /*max=*/50);
    EXPECT_EQ(exec->CandidateK(10, 1.0f), 30);  // 10*3
    EXPECT_EQ(exec->CandidateK(1, 1.0f), 3);    // 1*3
    EXPECT_EQ(exec->CandidateK(5, 1.0f), 15);   // 5*3
}

TEST_F(LiveExecutorTest, CandidateKCappedAtMax) {
    auto exec = MakeExec(/*multiplier=*/3, /*max=*/50);
    EXPECT_EQ(exec->CandidateK(20, 1.0f), 50);  // 20*3=60 capped to 50
    EXPECT_EQ(exec->CandidateK(100, 1.0f), 100);  // 100*3=300 capped to 50, then clamped UP to top_k=100
}

TEST_F(LiveExecutorTest, CandidateKOversampleCeil) {
    auto exec = MakeExec(/*multiplier=*/3, /*max=*/100);
    EXPECT_EQ(exec->CandidateK(10, 2.0f), 60);   // 10*3*ceil(2.0)=60
    EXPECT_EQ(exec->CandidateK(10, 1.5f), 60);   // ceil(1.5)=2 -> 10*3*2=60
    EXPECT_EQ(exec->CandidateK(10, 1.0f), 30);   // oversample<=1 -> factor 1
    EXPECT_EQ(exec->CandidateK(10, 0.5f), 30);   // oversample<1 still factor 1 (no shrink)
}

TEST_F(LiveExecutorTest, CandidateKNeverBelowTopK) {
    // multiplier 1, tiny cap: cand would be < top_k, must clamp UP to top_k.
    auto exec = MakeExec(/*multiplier=*/1, /*max=*/5);
    EXPECT_EQ(exec->CandidateK(20, 1.0f), 20);  // 20*1=20 > cap5 -> capped 5 -> clamped up to 20
    EXPECT_EQ(exec->CandidateK(3, 1.0f), 3);    // 3*1=3 <= cap5 -> 3
}

TEST_F(LiveExecutorTest, CandidateKTopKFloorAtOne) {
    auto exec = MakeExec(/*multiplier=*/3, /*max=*/50);
    EXPECT_EQ(exec->CandidateK(0, 1.0f), 3);    // top_k<1 -> treated as 1 -> 1*3
    EXPECT_EQ(exec->CandidateK(-5, 1.0f), 3);   // negative top_k -> 1 -> 3
}

TEST_F(LiveExecutorTest, CandidateKCtorClampsMultiplierAndMax) {
    // ctor clamps multiplier<1 -> 1 and max<1 -> 1.
    auto exec = MakeExec(/*multiplier=*/0, /*max=*/0);
    // multiplier->1, max->1: cand = top_k*1, capped at 1, clamped up to top_k.
    EXPECT_EQ(exec->CandidateK(7, 1.0f), 7);   // 7*1=7 > cap1 -> 1 -> clamp up to 7
    EXPECT_EQ(exec->CandidateK(1, 1.0f), 1);   // 1*1=1
}

// --- FlattenMetadataIntoMap: depth-guard + edge branches ----------------------

// At-limit metadata flattens normally; over-limit is SKIPPED whole (the DoS guard
// returns before the dump() that could overflow the stack).
TEST_F(LiveExecutorTest, FlattenSkipsOverDepthMetadata) {
    // Wrap a deep value as one top-level key so the object is flattenable in shape
    // but its value's depth trips the guard. depth = kMaxMetadataDepth+2 inside "k".
    const std::string deep_inner = DeepObjectJson(kMaxMetadataDepth + 2);
    const std::string md = std::string("{\"k\":") + deep_inner + "}";
    std::map<std::string, std::string> out;
    out["pre"] = "kept";
    EXPECT_NO_THROW(FlattenMetadataIntoMap(md, out));
    // Over-depth -> whole metadata skipped; pre-existing entry untouched, no "k".
    EXPECT_EQ(out.count("k"), 0u);
    EXPECT_EQ(out["pre"], "kept");
    EXPECT_EQ(out.size(), 1u);
}

// A within-limit nested value flattens (serialized via dump()).
TEST_F(LiveExecutorTest, FlattenKeepsWithinLimitNested) {
    std::map<std::string, std::string> out;
    FlattenMetadataIntoMap(R"({"meta":{"a":{"b":"c"}}})", out);
    EXPECT_EQ(out["meta"], R"({"a":{"b":"c"}})");
}

// Empty / non-object / invalid JSON are non-destructive no-ops.
TEST_F(LiveExecutorTest, FlattenToleratesEmptyNonObjectInvalid) {
    std::map<std::string, std::string> out;
    out["x"] = "1";
    EXPECT_NO_THROW(FlattenMetadataIntoMap("", out));
    EXPECT_NO_THROW(FlattenMetadataIntoMap("[1,2,3]", out));     // array
    EXPECT_NO_THROW(FlattenMetadataIntoMap("\"scalar\"", out));  // bare string
    EXPECT_NO_THROW(FlattenMetadataIntoMap("{bad json", out));   // parse error
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out["x"], "1");
}

// --- ClassifyVectorHit: the §3.8 W2 vector-route split (pure function) --------

TEST_F(LiveExecutorTest, ClassifyVectorHitDenseUsesOwnChildId) {
    std::string child;
    EXPECT_EQ(ClassifyVectorHit(/*block_type=*/1, "01CHILD", "{}", &child),
              VectorHitPath::kDense);
    EXPECT_EQ(child, "01CHILD");
}

TEST_F(LiveExecutorTest, ClassifyVectorHitEmptyChildDropped) {
    std::string child;
    EXPECT_EQ(ClassifyVectorHit(/*block_type=*/1, "", "{}", &child),
              VectorHitPath::kDropped);
}

TEST_F(LiveExecutorTest, ClassifyVectorHitHypeExpandsToSourceChild) {
    std::string child;
    // Real hype rows have an EMPTY own child_id; the vote must go to the source.
    EXPECT_EQ(ClassifyVectorHit(/*block_type=*/16, "",
                                R"({"source_child_id":"01SRC","question_text":"q?"})",
                                &child),
              VectorHitPath::kHype);
    EXPECT_EQ(child, "01SRC");
}

TEST_F(LiveExecutorTest, ClassifyVectorHitHypeWithoutSourceDropped) {
    std::string child;
    // Missing key / empty value / non-string / invalid JSON: all dropped — the
    // question text must never impersonate a chunk.
    EXPECT_EQ(ClassifyVectorHit(16, "", "{}", &child), VectorHitPath::kDropped);
    EXPECT_EQ(ClassifyVectorHit(16, "", R"({"source_child_id":""})", &child),
              VectorHitPath::kDropped);
    EXPECT_EQ(ClassifyVectorHit(16, "", R"({"source_child_id":7})", &child),
              VectorHitPath::kDropped);
    EXPECT_EQ(ClassifyVectorHit(16, "", "{bad", &child), VectorHitPath::kDropped);
}

TEST_F(LiveExecutorTest, DerivedVoteResolutionPreservesSourceIdentity) {
    CortrixStoreSqlite store(":memory:");
    ASSERT_EQ(store.Open(), 0);

    CortrixDoc doc;
    doc.source_type = "unit";
    doc.source_path = "source-a.md";
    doc.metadata_json = R"({"input_id":"source-a","scenario":"duplicate-text"})";
    ASSERT_EQ(store.doc_create(doc), 0);

    CortrixBlock source;
    source.child_id = "01SOURCECHILD0000000000001";
    source.block_id = id::HashChildIdToBlockId(source.child_id);
    source.doc_id = doc.doc_id;
    source.parent_id = "01SOURCEPARENT000000000001";
    source.block_type = kBlockFile;
    source.processing_level = kLevelL3;
    source.content_text = "Repeated passage shared by two source documents.";
    source.metadata_json = R"({"chunk_source":"source-a"})";
    source.data = BlockBuild(kBlockFile, kLevelL3, source.content_text,
                             source.metadata_json, "", 0, 0);
    ASSERT_EQ(store.block_insert(source), 0);

    CortrixBlock resolved;
    ASSERT_TRUE(ResolveSourceChildBlock(store, source.child_id, &resolved));
    EXPECT_EQ(resolved.child_id, source.child_id);
    EXPECT_EQ(resolved.doc_id, doc.doc_id);
    EXPECT_EQ(resolved.parent_id, source.parent_id);
    EXPECT_EQ(resolved.metadata_json, source.metadata_json);
    EXPECT_EQ(resolved.content_text, source.content_text);
}

TEST_F(LiveExecutorTest, DerivedVoteResolutionKeepsLegacyLookupCompatible) {
    CortrixStoreSqlite store(":memory:");
    ASSERT_EQ(store.Open(), 0);

    CortrixDoc doc;
    doc.source_type = "unit";
    doc.source_path = "legacy-source.md";
    ASSERT_EQ(store.doc_create(doc), 0);

    CortrixBlock legacy;
    legacy.child_id = "01LEGACYCHILD0000000000001";
    legacy.parent_id = "01LEGACYPARENT00000000001";
    legacy.doc_id = doc.doc_id;
    legacy.chunk_index = 7;
    legacy.block_type = kBlockFile;
    legacy.processing_level = kLevelL3;
    legacy.content_text = "Legacy source content.";
    legacy.data = BlockBuild(kBlockFile, kLevelL3, legacy.content_text, "", "", 0, 0);
    ASSERT_EQ(store.block_insert(legacy), 0);
    ASSERT_NE(legacy.block_id, id::HashChildIdToBlockId(legacy.child_id));

    CortrixBlock resolved;
    ASSERT_TRUE(ResolveSourceChildBlock(store, legacy.child_id, &resolved));
    EXPECT_EQ(resolved.child_id, legacy.child_id);
    EXPECT_EQ(resolved.parent_id, legacy.parent_id);
    EXPECT_EQ(resolved.chunk_index, legacy.chunk_index);
    EXPECT_EQ(resolved.content_text, legacy.content_text);
}

// String values are kept verbatim (no surrounding quotes); non-strings dump().
TEST_F(LiveExecutorTest, FlattenStringVerbatimNonStringDumped) {
    std::map<std::string, std::string> out;
    FlattenMetadataIntoMap(
        R"({"id":"abc","n":42,"f":1.5,"b":true,"arr":[1,2]})", out);
    EXPECT_EQ(out["id"], "abc");      // raw, not "\"abc\""
    EXPECT_EQ(out["n"], "42");
    EXPECT_EQ(out["f"], "1.5");
    EXPECT_EQ(out["b"], "true");
    EXPECT_EQ(out["arr"], "[1,2]");
}

// On a key collision the metadata field overrides any pre-existing entry (it is
// the document's own field — same contract as the scatter-side test).
TEST_F(LiveExecutorTest, FlattenMetadataOverridesOnCollision) {
    std::map<std::string, std::string> out;
    out["source"] = "seed";
    FlattenMetadataIntoMap(R"({"source":"doc"})", out);
    EXPECT_EQ(out["source"], "doc");
}

// --- FuseHybridChunksByDocId: granularity=auto/both candidate fusion ----------

retrieval::RankedChunk MakeChunkHit(const std::string& child_id,
                                    const std::string& doc_id,
                                    float score) {
    retrieval::RankedChunk rc;
    rc.child_id = child_id;
    rc.chunk_text = "chunk text " + child_id;
    rc.score = score;
    rc.rerank_score = score;
    rc.metadata["doc_id"] = doc_id;
    rc.metadata["source_doc_id"] = doc_id;
    return rc;
}

retrieval::RankedChunk MakeDocHit(const std::string& doc_id,
                                  const std::string& via_path,
                                  float score) {
    retrieval::RankedChunk rc;
    rc.child_id = doc_id;
    rc.chunk_text = "doc text " + doc_id;
    rc.score = score;
    rc.rerank_score = score;
    rc.metadata["doc_id"] = doc_id;
    rc.metadata["source_doc_id"] = doc_id;
    rc.metadata["via_path"] = via_path;
    rc.metadata["doc_discovery_match_score"] = std::to_string(score);
    rc.metadata["one_liner"] = "doc one liner";
    return rc;
}

TEST(LiveHybridFusionTest, FusesChunkAndDocEvidenceByDocIdNotChildId) {
    std::vector<retrieval::RankedChunk> chunks;
    chunks.push_back(MakeChunkHit("child-1", "doc-1", 0.90f));

    std::vector<retrieval::RankedChunk> docs;
    docs.push_back(MakeDocHit("doc-1", "llm_summary", 0.80f));

    std::vector<retrieval::RankedChunk> fused =
        FuseHybridChunksByDocId(std::move(chunks), std::move(docs), 10);

    ASSERT_EQ(fused.size(), 1u);
    EXPECT_EQ(fused[0].child_id, "child-1");  // keep actual chunk representative
    EXPECT_EQ(fused[0].metadata["doc_id"], "doc-1");
    EXPECT_EQ(fused[0].metadata["source_doc_id"], "doc-1");
    EXPECT_EQ(fused[0].metadata["via_path"], "hybrid");
    EXPECT_EQ(fused[0].metadata["hybrid_paths"], "chunk,llm_summary");
    EXPECT_EQ(fused[0].metadata["hybrid_has_chunk"], "true");
    EXPECT_EQ(fused[0].metadata["hybrid_has_doc"], "true");
    EXPECT_EQ(fused[0].metadata["one_liner"], "doc one liner");
    EXPECT_NEAR(fused[0].score, (1.0f / 60.0f) + (1.0f / 60.0f), 1e-6f);
}

TEST(LiveHybridFusionTest, PreservesDocOnlyFts5FallbackCandidate) {
    std::vector<retrieval::RankedChunk> chunks;

    std::vector<retrieval::RankedChunk> docs;
    docs.push_back(MakeDocHit("doc-fts", "fts5_fallback", 0.50f));

    std::vector<retrieval::RankedChunk> fused =
        FuseHybridChunksByDocId(std::move(chunks), std::move(docs), 10);

    ASSERT_EQ(fused.size(), 1u);
    EXPECT_EQ(fused[0].child_id, "doc-fts");
    EXPECT_EQ(fused[0].metadata["via_path"], "fts5_fallback");
    EXPECT_EQ(fused[0].metadata["hybrid_paths"], "fts5_fallback");
    EXPECT_EQ(fused[0].metadata["hybrid_has_chunk"], "false");
    EXPECT_EQ(fused[0].metadata["hybrid_has_doc"], "true");
}

TEST(LiveHybridFusionTest, OrdersByDocLevelRrfAndTruncatesTopK) {
    std::vector<retrieval::RankedChunk> chunks;
    chunks.push_back(MakeChunkHit("child-a", "doc-a", 0.90f));
    chunks.push_back(MakeChunkHit("child-b", "doc-b", 0.80f));

    std::vector<retrieval::RankedChunk> docs;
    docs.push_back(MakeDocHit("doc-b", "llm_summary", 0.70f));
    docs.push_back(MakeDocHit("doc-c", "llm_summary", 0.60f));

    std::vector<retrieval::RankedChunk> fused =
        FuseHybridChunksByDocId(std::move(chunks), std::move(docs), 2);

    ASSERT_EQ(fused.size(), 2u);
    EXPECT_EQ(fused[0].metadata["doc_id"], "doc-b");  // two-path evidence wins
    EXPECT_EQ(fused[0].metadata["via_path"], "hybrid");
    EXPECT_EQ(fused[1].metadata["doc_id"], "doc-a");
}

TEST(LiveHybridFusionTest, KeepsBestChunkRepresentativeForSameDoc) {
    std::vector<retrieval::RankedChunk> chunks;
    chunks.push_back(MakeChunkHit("child-best", "doc-1", 0.95f));
    chunks.push_back(MakeChunkHit("child-later", "doc-1", 0.50f));

    std::vector<retrieval::RankedChunk> docs;
    docs.push_back(MakeDocHit("doc-1", "llm_summary", 0.80f));

    std::vector<retrieval::RankedChunk> fused =
        FuseHybridChunksByDocId(std::move(chunks), std::move(docs), 10);

    ASSERT_EQ(fused.size(), 1u);
    EXPECT_EQ(fused[0].child_id, "child-best");
    EXPECT_EQ(fused[0].metadata["doc_id"], "doc-1");
    EXPECT_EQ(fused[0].metadata["hybrid_paths"], "chunk,llm_summary");
    EXPECT_EQ(fused[0].metadata["via_path"], "hybrid");
}

TEST(LiveHybridFusionTest, DocEvidenceDoesNotOverrideChunkIdentityOrSourcePath) {
    std::vector<retrieval::RankedChunk> chunks;
    retrieval::RankedChunk chunk = MakeChunkHit("child-1", "doc-1", 0.90f);
    chunk.metadata["source_path"] = "chunk-source.pdf";
    chunk.metadata["one_liner"] = "chunk one liner";
    chunks.push_back(std::move(chunk));

    std::vector<retrieval::RankedChunk> docs;
    retrieval::RankedChunk doc = MakeDocHit("doc-1", "llm_summary", 0.80f);
    doc.metadata["source_path"] = "doc-source.pdf";
    doc.metadata["one_liner"] = "doc one liner";
    docs.push_back(std::move(doc));

    std::vector<retrieval::RankedChunk> fused =
        FuseHybridChunksByDocId(std::move(chunks), std::move(docs), 10);

    ASSERT_EQ(fused.size(), 1u);
    EXPECT_EQ(fused[0].child_id, "child-1");
    EXPECT_EQ(fused[0].metadata["doc_id"], "doc-1");
    EXPECT_EQ(fused[0].metadata["source_doc_id"], "doc-1");
    EXPECT_EQ(fused[0].metadata["source_path"], "chunk-source.pdf");
    EXPECT_EQ(fused[0].metadata["one_liner"], "doc one liner");
}

TEST(LiveHybridFusionTest, TopKFloorReturnsAtLeastOneCandidate) {
    std::vector<retrieval::RankedChunk> chunks;
    chunks.push_back(MakeChunkHit("child-a", "doc-a", 0.90f));
    chunks.push_back(MakeChunkHit("child-b", "doc-b", 0.80f));

    std::vector<retrieval::RankedChunk> docs;
    docs.push_back(MakeDocHit("doc-b", "llm_summary", 0.70f));

    std::vector<retrieval::RankedChunk> fused =
        FuseHybridChunksByDocId(std::move(chunks), std::move(docs), 0);

    ASSERT_EQ(fused.size(), 1u);
    EXPECT_EQ(fused[0].metadata["doc_id"], "doc-b");
}

}  // namespace
}  // namespace cortrix::query
