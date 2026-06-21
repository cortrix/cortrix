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

#include "cortrix/common/json_depth.h"
#include "cortrix/query/live_single_unit_executor.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"

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

}  // namespace
}  // namespace cortrix::query
