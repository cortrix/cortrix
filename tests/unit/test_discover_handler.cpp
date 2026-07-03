// F41 doc-discovery HTTP handler + orchestration core
// (src/doc_summary/discover_handler.cpp). Deterministic GoogleTest cases for:
//   - HandleDocumentsDiscover param validation (query / ns|namespace / top_k / explain)
//   - ExecuteDocDiscovery degrade paths (Acquire fail / HNSW throw / fts5 gate / fts5
//     fault / explain.fallback_triggered / top_k clamp / happy coverage_ratio)
//   - RecallDocSummaryHnsw filtering / parsing / over-fetch / sort
//   - Shared SearchDocFts5 fallback + file-local HitToJson exercised THROUGH
//     ExecuteDocDiscovery.
//
// All inputs are deterministic: a stub OnnxEmbedder (no real model / no Init() => an
// embed-failure double), an in-memory FakeDocStore, a configurable FakeDiscoveryIndex,
// and a real DefaultNamespacePool over a temp dir (DiscoveryPoolHarness).
#include "cortrix/doc_summary/discover_handler.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib.h"

#include "cortrix/doc_summary/doc_summary_generator.h"  // DocSummaryConfig
#include "cortrix/doc_summary/doc_summary_metrics.h"
#include "cortrix/server/request_context.h"
#include "cortrix/spc/onnx_embedder.h"

#include "fake_doc_discovery_deps.h"

namespace cortrix::doc_summary {
namespace {

using cortrix::doc_summary::test::DiscoveryPoolHarness;
using cortrix::doc_summary::test::FakeDiscoveryIndex;
using cortrix::doc_summary::test::FakeDocStore;

// ------------------------- embedder doubles ------------------------------
// Happy embedder: real stub model, Init()'d => Embed returns a non-empty vector.
std::unique_ptr<OnnxEmbedder> MakeOkEmbedder(int dim = 128) {
    auto e = std::make_unique<OnnxEmbedder>("", dim);
    e->Init();
    return e;
}
// Empty-vector embedder: dim 0 => StubEmbed yields an empty vector (early return).
std::unique_ptr<OnnxEmbedder> MakeEmptyVecEmbedder() {
    auto e = std::make_unique<OnnxEmbedder>("", 0);
    e->Init();
    return e;
}
// Embed-failure embedder: never Init()'d => Embed returns a non-ok Status.
std::unique_ptr<OnnxEmbedder> MakeFailEmbedder() {
    return std::make_unique<OnnxEmbedder>("", 128);  // no Init()
}

// A valid S4.2 metadata_json for a "generated" doc_summary block.
std::string Meta(const std::string& status = "generated") {
    nlohmann::json m;
    m["status"] = status;
    m["keywords"] = {"revenue", "finance"};
    m["topics"] = {"Financial Report"};
    m["one_liner"] = "Q3 financials";
    return m.dump();
}

// ===========================================================================
// Section A - HandleDocumentsDiscover param validation
// ===========================================================================
class HandleParamTest : public ::testing::Test {
protected:
    void SetUp() override {
        DocSummaryMetrics::Instance().ResetForTest();
        harness_ = std::make_unique<DiscoveryPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("discover_param_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        ASSERT_TRUE(harness_->Admit("ns1").ok());
        embedder_ = MakeOkEmbedder();
    }

    httplib::Request Req(
        const std::vector<std::pair<std::string, std::string>>& params) {
        httplib::Request r;
        for (const auto& [k, v] : params) r.params.emplace(k, v);
        return r;
    }

    void Call(const httplib::Request& req, httplib::Response& res) {
        cortrix::RequestContext rctx;
        rctx.request_id = "req-test";
        HandleDocumentsDiscover(req, res, rctx, harness_->ipool(), *embedder_,
                                config_);
    }

    std::unique_ptr<DiscoveryPoolHarness> harness_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    DocSummaryConfig config_;
};

TEST_F(HandleParamTest, MissingQueryReturns400) {
    httplib::Response res;
    Call(Req({{"ns", "ns1"}}), res);
    EXPECT_EQ(res.status, 400);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(HandleParamTest, EmptyQueryReturns400) {
    httplib::Response res;
    Call(Req({{"query", ""}, {"ns", "ns1"}}), res);
    EXPECT_EQ(res.status, 400);
    EXPECT_EQ(nlohmann::json::parse(res.body)["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(HandleParamTest, MissingBothNsAndNamespaceReturns400) {
    httplib::Response res;
    Call(Req({{"query", "hello"}}), res);
    EXPECT_EQ(res.status, 400);
    EXPECT_EQ(nlohmann::json::parse(res.body)["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(HandleParamTest, EmptyNsValueReturns400) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", ""}}), res);
    EXPECT_EQ(res.status, 400);
}

TEST_F(HandleParamTest, NsParamAccepted) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}}), res);
    EXPECT_EQ(res.status, 200);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_TRUE(body.contains("results"));
    EXPECT_TRUE(body.contains("meta"));
}

TEST_F(HandleParamTest, NamespaceAliasAccepted) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"namespace", "ns1"}}), res);
    EXPECT_EQ(res.status, 200);
}

TEST_F(HandleParamTest, NsTakesPrecedenceWhenBothPresent) {
    // ns is checked first; "ns1" is admitted, the namespace alias points elsewhere.
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"namespace", "ghost"}}), res);
    EXPECT_EQ(res.status, 200);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["meta"]["coverage_ratio"], 1.0);  // ns1 acquired, not the ghost
}

TEST_F(HandleParamTest, NonIntegerTopKReturns400) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"top_k", "abc"}}), res);
    EXPECT_EQ(res.status, 400);
    EXPECT_EQ(nlohmann::json::parse(res.body)["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(HandleParamTest, TopKZeroReturns400) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"top_k", "0"}}), res);
    EXPECT_EQ(res.status, 400);
}

TEST_F(HandleParamTest, TopKNegativeReturns400) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"top_k", "-5"}}), res);
    EXPECT_EQ(res.status, 400);
}

TEST_F(HandleParamTest, TopKTooLargeReturns400) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"top_k", "101"}}), res);
    EXPECT_EQ(res.status, 400);
}

TEST_F(HandleParamTest, TopKOneAccepted) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"top_k", "1"}}), res);
    EXPECT_EQ(res.status, 200);
}

TEST_F(HandleParamTest, TopKHundredAccepted) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"top_k", "100"}}), res);
    EXPECT_EQ(res.status, 200);
}

TEST_F(HandleParamTest, TopKDefaultWhenAbsent) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}}), res);  // no top_k => default 10
    EXPECT_EQ(res.status, 200);
}

TEST_F(HandleParamTest, ExplainTrueLowercaseAccepted) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"explain", "true"}}), res);
    EXPECT_EQ(res.status, 200);
}

TEST_F(HandleParamTest, ExplainOneAccepted) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"explain", "1"}}), res);
    EXPECT_EQ(res.status, 200);
}

TEST_F(HandleParamTest, ExplainFalsyDoesNotAddFallbackMeta) {
    // explain=false + no fallback contribution => no fallback_triggered key.
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"explain", "false"}}), res);
    EXPECT_EQ(res.status, 200);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_FALSE(body["meta"].contains("fallback_triggered"));
}

TEST_F(HandleParamTest, ExplainGarbageTreatedAsFalsy) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}, {"explain", "yes"}}), res);
    EXPECT_EQ(res.status, 200);
    EXPECT_FALSE(nlohmann::json::parse(res.body)["meta"].contains("fallback_triggered"));
}

TEST_F(HandleParamTest, HappyResponseHasPartialSuccessMetaShape) {
    httplib::Response res;
    Call(Req({{"query", "hello"}, {"ns", "ns1"}}), res);
    ASSERT_EQ(res.status, 200);
    auto meta = nlohmann::json::parse(res.body)["meta"];
    EXPECT_EQ(meta["succeeded"].size(), 1u);
    EXPECT_EQ(meta["failed"].size(), 0u);
    EXPECT_EQ(meta["coverage_ratio"], 1.0);
    EXPECT_TRUE(meta.contains("warnings"));
}

// ===========================================================================
// Section B - ExecuteDocDiscovery orchestration / degrade paths
// ===========================================================================
class ExecuteTest : public ::testing::Test {
protected:
    void SetUp() override {
        DocSummaryMetrics::Instance().ResetForTest();
        harness_ = std::make_unique<DiscoveryPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("discover_exec_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        ASSERT_TRUE(harness_->Admit("ns1").ok());
        embedder_ = MakeOkEmbedder();
    }

    std::unique_ptr<DiscoveryPoolHarness> harness_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    DocSummaryConfig config_;
};

TEST_F(ExecuteTest, AcquireFailureGivesZeroCoverageAndFailedNs) {
    // "missing" was never admitted => pool.Acquire fails => partial-success shape.
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "missing",
                                             "q", 10, false, config_);
    EXPECT_TRUE(out["results"].is_array());
    EXPECT_EQ(out["results"].size(), 0u);
    EXPECT_EQ(out["meta"]["coverage_ratio"], 0.0);
    ASSERT_EQ(out["meta"]["failed"].size(), 1u);
    EXPECT_EQ(out["meta"]["failed"][0], "missing");
    EXPECT_EQ(out["meta"]["succeeded"].size(), 0u);
}

TEST_F(ExecuteTest, AcquireFailureDoesNotThrow) {
    EXPECT_NO_THROW({
        ExecuteDocDiscovery(harness_->ipool(), *embedder_, "missing", "q", 10, false,
                            config_);
    });
}

TEST_F(ExecuteTest, HappyPathCoverageRatioOne) {
    uint64_t bid = harness_->SeedDocSummaryBlock("ns1", "docA", "summary A", Meta());
    harness_->fake_index()->set_search_result({{bid, 0.1f}});
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1", "q",
                                             10, false, config_);
    EXPECT_EQ(out["meta"]["coverage_ratio"], 1.0);
    EXPECT_EQ(out["meta"]["succeeded"].size(), 1u);
    ASSERT_EQ(out["results"].size(), 1u);
    EXPECT_EQ(out["results"][0]["doc_id"], "docA");
    EXPECT_EQ(out["results"][0]["via_path"], "llm_summary");
}

TEST_F(ExecuteTest, HnswSearchThrowAddsWarningAndDegrades) {
    harness_->fake_index()->set_search_throws(true);
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1", "q",
                                             10, false, config_);
    // No throw escapes; main path empty; a warning records the fault.
    EXPECT_EQ(out["meta"]["coverage_ratio"], 1.0);
    ASSERT_FALSE(out["meta"]["warnings"].empty());
    bool found = false;
    for (const auto& w : out["meta"]["warnings"])
        if (w.get<std::string>().find("doc_summary_hnsw_failed") != std::string::npos)
            found = true;
    EXPECT_TRUE(found);
}

TEST_F(ExecuteTest, Fts5DisabledSkipsFallback) {
    config_.fts5_fallback_enabled = false;
    harness_->CreateFts5Table("ns1", "docF", "f.pdf", "Quarterly Finance Report",
                              "finance", "");
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1",
                                             "finance", 10, true, config_);
    // With fts5 disabled, the fts5 doc must not appear and no fallback_triggered.
    EXPECT_FALSE(out["meta"].contains("fallback_triggered"));
    for (const auto& r : out["results"])
        EXPECT_NE(r["via_path"], "fts5_fallback");
}

TEST_F(ExecuteTest, Fts5EnabledFallbackDocAppears) {
    config_.fts5_fallback_enabled = true;
    harness_->CreateFts5Table("ns1", "docF", "f.pdf", "Quarterly Finance Report",
                              "finance revenue", "");
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1",
                                             "finance", 10, true, config_);
    bool found = false;
    for (const auto& r : out["results"]) {
        if (r["doc_id"] == "docF") {
            found = true;
            EXPECT_EQ(r["via_path"], "fts5_fallback");
            EXPECT_EQ(r["doc_title"], "Quarterly Finance Report");
            EXPECT_EQ(r["filename"], "f.pdf");
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ExecuteTest, ExplainAddsFallbackTriggeredWhenFallbackContributes) {
    config_.fts5_fallback_enabled = true;
    harness_->CreateFts5Table("ns1", "docF", "f.pdf", "Finance Report", "finance", "");
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1",
                                             "finance", 10, /*explain=*/true, config_);
    ASSERT_TRUE(out["meta"].contains("fallback_triggered"));
    EXPECT_EQ(out["meta"]["fallback_triggered"], true);
    EXPECT_GE(out["meta"]["fallback_docs_count"].get<int>(), 1);
}

TEST_F(ExecuteTest, ExplainNoFallbackTriggeredWhenFallbackEmpty) {
    // fts5 enabled but the table has no matching row => fallback contributes nothing.
    config_.fts5_fallback_enabled = true;
    harness_->CreateFts5Table("ns1", "docF", "f.pdf", "Unrelated Title", "cooking", "");
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1",
                                             "finance", 10, /*explain=*/true, config_);
    EXPECT_FALSE(out["meta"].contains("fallback_triggered"));
}

TEST_F(ExecuteTest, Fts5EmptyIndexIsSilentNoOpAndMainResultSurvives) {
    // The per-Unit store auto-creates doc_fts5_index via the F41 schema provider, so a
    // query that simply matches no rows is a SILENT no-op (not a prepare-fault): the
    // fallback-failed metric stays put, no fallback_triggered is emitted, and the main
    // HNSW result still surfaces. (A genuine SQLite fault would need fault injection;
    // that degrade path is covered at the store-seam level elsewhere.)
    config_.fts5_fallback_enabled = true;
    uint64_t bid = harness_->SeedDocSummaryBlock("ns1", "docA", "summary A", Meta());
    harness_->fake_index()->set_search_result({{bid, 0.2f}});

    uint64_t before = DocSummaryMetrics::Instance().Fts5FallbackFailedCount();
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1", "q",
                                             10, /*explain=*/true, config_);
    EXPECT_EQ(DocSummaryMetrics::Instance().Fts5FallbackFailedCount(), before);  // no fault
    // Main HNSW result survives regardless of the empty fts5 path.
    ASSERT_EQ(out["results"].size(), 1u);
    EXPECT_EQ(out["results"][0]["doc_id"], "docA");
    // fts5 contributed nothing and did not fault => fallback_triggered is absent.
    EXPECT_FALSE(out["meta"].contains("fallback_triggered"));
}

TEST_F(ExecuteTest, TopKBelowOneClampedToOne) {
    // Seed two doc_summary blocks; top_k=0 is clamped to 1 => at most 1 result.
    uint64_t b1 = harness_->SeedDocSummaryBlock("ns1", "d1", "s1", Meta());
    uint64_t b2 = harness_->SeedDocSummaryBlock("ns1", "d2", "s2", Meta());
    harness_->fake_index()->set_search_result({{b1, 0.1f}, {b2, 0.2f}});
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1", "q",
                                             0, false, config_);
    EXPECT_EQ(out["results"].size(), 1u);
}

TEST_F(ExecuteTest, EmptyVectorEmbedderYieldsNoLlmResults) {
    auto empty_emb = MakeEmptyVecEmbedder();
    uint64_t bid = harness_->SeedDocSummaryBlock("ns1", "d1", "s1", Meta());
    harness_->fake_index()->set_search_result({{bid, 0.1f}});
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *empty_emb, "ns1", "q",
                                             10, false, config_);
    EXPECT_EQ(out["meta"]["coverage_ratio"], 1.0);  // still a success shape
    EXPECT_EQ(out["results"].size(), 0u);           // no llm hits (empty embedding)
}

TEST_F(ExecuteTest, NoResultsStillPartialSuccess) {
    // Index returns nothing => empty results, coverage 1.0, ns succeeded.
    harness_->fake_index()->set_search_result({});
    nlohmann::json out = ExecuteDocDiscovery(harness_->ipool(), *embedder_, "ns1", "q",
                                             10, false, config_);
    EXPECT_EQ(out["results"].size(), 0u);
    EXPECT_EQ(out["meta"]["coverage_ratio"], 1.0);
    EXPECT_EQ(out["meta"]["succeeded"][0], "ns1");
}

// ===========================================================================
// Section C - RecallDocSummaryHnsw (pure core; standalone fakes)
// ===========================================================================
class RecallTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = MakeOkEmbedder();
    }
    FakeDiscoveryIndex index_;
    FakeDocStore store_;
    std::unique_ptr<OnnxEmbedder> embedder_;
};

TEST_F(RecallTest, TopKZeroReturnsEmpty) {
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 0);
    EXPECT_TRUE(out.empty());
}

TEST_F(RecallTest, TopKNegativeReturnsEmpty) {
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", -3);
    EXPECT_TRUE(out.empty());
}

TEST_F(RecallTest, EmbedFailureReturnsEmpty) {
    auto fail = MakeFailEmbedder();  // never Init()'d => Embed not ok
    index_.set_search_result({{1, 0.1f}});
    store_.Put(FakeDocStore::DocSummaryBlock(1, "d1", "s1", Meta()));
    auto out = RecallDocSummaryHnsw(index_, store_, *fail, "q", 10);
    EXPECT_TRUE(out.empty());
}

TEST_F(RecallTest, EmptyVectorReturnsEmpty) {
    auto empty_emb = MakeEmptyVecEmbedder();
    index_.set_search_result({{1, 0.1f}});
    store_.Put(FakeDocStore::DocSummaryBlock(1, "d1", "s1", Meta()));
    auto out = RecallDocSummaryHnsw(index_, store_, *empty_emb, "q", 10);
    EXPECT_TRUE(out.empty());
}

TEST_F(RecallTest, NonDocSummaryBlockFilteredOut) {
    cortrix::CortrixBlock b;
    b.block_id = 1;
    b.doc_id = "d1";
    b.block_type = 1;  // kBlockFile, NOT 17
    b.content_text = "file body";
    store_.Put(b);
    index_.set_search_result({{1, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    EXPECT_TRUE(out.empty());
}

TEST_F(RecallTest, MissingBlockSkipped) {
    // Index references block 99 that the store does not have => block_get != 0 skip.
    index_.set_search_result({{99, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    EXPECT_TRUE(out.empty());
}

TEST_F(RecallTest, StatusNotGeneratedSkipped) {
    store_.Put(FakeDocStore::DocSummaryBlock(1, "d1", "s1", Meta("pending")));
    index_.set_search_result({{1, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    EXPECT_TRUE(out.empty());
}

TEST_F(RecallTest, StatusFailedSkipped) {
    store_.Put(FakeDocStore::DocSummaryBlock(1, "d1", "s1", Meta("failed")));
    index_.set_search_result({{1, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    EXPECT_TRUE(out.empty());
}

TEST_F(RecallTest, GeneratedBlockProducesHitWithStructuredFields) {
    store_.Put(FakeDocStore::DocSummaryBlock(1, "d1", "the summary text", Meta()));
    index_.set_search_result({{1, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].doc_id, "d1");
    EXPECT_EQ(out[0].via_path, "llm_summary");
    EXPECT_EQ(out[0].summary_text, "the summary text");
    ASSERT_EQ(out[0].keywords.size(), 2u);
    EXPECT_EQ(out[0].topics.size(), 1u);
    EXPECT_EQ(out[0].one_liner, "Q3 financials");
}

TEST_F(RecallTest, EmptyMetadataKeepsSummaryText) {
    // No metadata_json => the if-block is skipped, status defaults to generated path.
    cortrix::CortrixBlock b = FakeDocStore::DocSummaryBlock(1, "d1", "body", "");
    store_.Put(b);
    index_.set_search_result({{1, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].summary_text, "body");
    EXPECT_TRUE(out[0].keywords.empty());
}

TEST_F(RecallTest, UnparseableMetadataKeepsSummaryTextViaCatch) {
    store_.Put(FakeDocStore::DocSummaryBlock(1, "d1", "body", "{not valid json"));
    index_.set_search_result({{1, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    ASSERT_EQ(out.size(), 1u);  // catch path keeps the hit
    EXPECT_EQ(out[0].doc_id, "d1");
    EXPECT_EQ(out[0].summary_text, "body");
    EXPECT_TRUE(out[0].keywords.empty());
}

TEST_F(RecallTest, MissingStatusKeyDefaultsToGenerated) {
    // metadata present but no "status" key => m.value("status","generated") keeps it.
    nlohmann::json m;
    m["keywords"] = {"a"};
    store_.Put(FakeDocStore::DocSummaryBlock(1, "d1", "body", m.dump()));
    index_.set_search_result({{1, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].keywords.size(), 1u);
}

TEST_F(RecallTest, ResultsTruncatedToTopK) {
    for (uint64_t i = 1; i <= 5; ++i)
        store_.Put(FakeDocStore::DocSummaryBlock(i, "d" + std::to_string(i),
                                                 "s", Meta()));
    index_.set_search_result(
        {{1, 0.1f}, {2, 0.2f}, {3, 0.3f}, {4, 0.4f}, {5, 0.5f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 2);
    EXPECT_EQ(out.size(), 2u);
}

TEST_F(RecallTest, OverFetchKeepsValidAfterFilteringNoise) {
    // Mix doc_summary(17) rows with non-17 rows; the over-fetch x4 lets the recall
    // still collect top_k=2 valid doc_summary hits despite the interleaved noise.
    store_.Put(FakeDocStore::DocSummaryBlock(1, "dA", "sA", Meta()));
    cortrix::CortrixBlock noise;
    noise.block_id = 2;
    noise.doc_id = "noise";
    noise.block_type = 16;  // kBlockHypeQuestion, filtered
    store_.Put(noise);
    store_.Put(FakeDocStore::DocSummaryBlock(3, "dB", "sB", Meta()));
    index_.set_search_result({{1, 0.1f}, {2, 0.15f}, {3, 0.2f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 2);
    ASSERT_EQ(out.size(), 2u);
    for (const auto& h : out) EXPECT_NE(h.doc_id, "noise");
}

TEST_F(RecallTest, SortedByMatchScoreDesc) {
    // distance 0.5 -> lower similarity than 0.1; result must be sorted score DESC.
    store_.Put(FakeDocStore::DocSummaryBlock(1, "low", "s", Meta()));
    store_.Put(FakeDocStore::DocSummaryBlock(2, "high", "s", Meta()));
    // Feed in the "wrong" order (low-score first) to prove the sort runs.
    index_.set_search_result({{1, 0.5f}, {2, 0.1f}});
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].doc_id, "high");
    EXPECT_GT(out[0].match_score, out[1].match_score);
}

TEST_F(RecallTest, MatchScoreFormulaFromDistance) {
    store_.Put(FakeDocStore::DocSummaryBlock(1, "d1", "s", Meta()));
    index_.set_search_result({{1, 1.0f}});  // 1/(1+1) = 0.5
    auto out = RecallDocSummaryHnsw(index_, store_, *embedder_, "q", 10);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].match_score, 0.5f);
}

}  // namespace
}  // namespace cortrix::doc_summary
