#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "cortrix/retrieval/sparse_rrf.h"

// F40 S8 — chunk-level 5-path RRF fusion (§9.1). dense + sparse + fts5 carry real
// structure; contextualized + hype are MOCK (empty lists) this round.
namespace cortrix::retrieval {
namespace {

std::vector<SparseHit> L(std::vector<std::string> ids) {
    std::vector<SparseHit> v;
    float s = 1.0f;
    for (auto& id : ids) v.push_back({id, s--});  // descending dummy scores
    return v;
}

bool HasPath(const RrfFusedHit& h, RrfPath p) {
    return std::find(h.contributing_paths.begin(), h.contributing_paths.end(), p) !=
           h.contributing_paths.end();
}

const RrfFusedHit* Find(const std::vector<RrfFusedHit>& v, const std::string& id) {
    for (const auto& h : v) if (h.child_id == id) return &h;
    return nullptr;
}

// ---------- path labels ----------

TEST(F40SparseRrfTest, PathLabels) {
    EXPECT_STREQ(ToString(RrfPath::kDense), "dense");
    EXPECT_STREQ(ToString(RrfPath::kContextualized), "contextualized");
    EXPECT_STREQ(ToString(RrfPath::kSparse), "sparse");
    EXPECT_STREQ(ToString(RrfPath::kFts5), "fts5");
    EXPECT_STREQ(ToString(RrfPath::kHypeQuestion), "hype_question");
    EXPECT_EQ(kRrfPathCount, 5);
    EXPECT_EQ(kRrfKDefault, 60);
}

// ---------- single path ----------

TEST(F40SparseRrfTest, SinglePathRrfScore) {
    FivePathInput in;
    in.sparse = L({"a", "b", "c"});
    auto out = FuseFivePathRrf(in);
    ASSERT_EQ(out.size(), 3u);
    // rank0 → 1/(60+0); rank1 → 1/61; rank2 → 1/62.
    EXPECT_EQ(out[0].child_id, "a");
    EXPECT_FLOAT_EQ(out[0].rrf_score, 1.0f / 60.0f);
    EXPECT_FLOAT_EQ(out[1].rrf_score, 1.0f / 61.0f);
    EXPECT_FLOAT_EQ(out[2].rrf_score, 1.0f / 62.0f);
}

// ---------- multi-path accumulation ----------

TEST(F40SparseRrfTest, ChildInMultiplePathsAccumulates) {
    FivePathInput in;
    in.dense = L({"x", "y"});   // x rank0, y rank1
    in.sparse = L({"y", "x"});  // y rank0, x rank1
    in.fts5 = L({"x"});         // x rank0
    auto out = FuseFivePathRrf(in);
    ASSERT_EQ(out.size(), 2u);
    const RrfFusedHit* x = Find(out, "x");
    const RrfFusedHit* y = Find(out, "y");
    ASSERT_TRUE(x && y);
    // x: dense r0 + sparse r1 + fts5 r0 = 1/60 + 1/61 + 1/60
    EXPECT_FLOAT_EQ(x->rrf_score, 1.0f / 60 + 1.0f / 61 + 1.0f / 60);
    // y: dense r1 + sparse r0 = 1/61 + 1/60
    EXPECT_FLOAT_EQ(y->rrf_score, 1.0f / 61 + 1.0f / 60);
    EXPECT_GT(x->rrf_score, y->rrf_score);  // x ranks first
    EXPECT_EQ(out[0].child_id, "x");
}

TEST(F40SparseRrfTest, ContributingPathsRecorded) {
    FivePathInput in;
    in.dense = L({"a"});
    in.sparse = L({"a"});
    in.fts5 = L({"b"});
    auto out = FuseFivePathRrf(in);
    const RrfFusedHit* a = Find(out, "a");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->contributing_paths.size(), 2u);
    EXPECT_TRUE(HasPath(*a, RrfPath::kDense));
    EXPECT_TRUE(HasPath(*a, RrfPath::kSparse));
    EXPECT_FALSE(HasPath(*a, RrfPath::kFts5));
    const RrfFusedHit* b = Find(out, "b");
    ASSERT_TRUE(b);
    EXPECT_TRUE(HasPath(*b, RrfPath::kFts5));
}

// ---------- F35/F38 mock paths (empty this round) ----------

TEST(F40SparseRrfTest, MockContextualizedAndHypeEmptyAreNoOps) {
    FivePathInput in;
    in.dense = L({"a"});
    in.sparse = L({"a"});
    // contextualized + hype intentionally empty (mock this round).
    auto out = FuseFivePathRrf(in);
    ASSERT_EQ(out.size(), 1u);
    const RrfFusedHit* a = Find(out, "a");
    ASSERT_TRUE(a);
    EXPECT_FALSE(HasPath(*a, RrfPath::kContextualized));
    EXPECT_FALSE(HasPath(*a, RrfPath::kHypeQuestion));
    EXPECT_EQ(a->contributing_paths.size(), 2u);  // only dense+sparse ran
}

TEST(F40SparseRrfTest, MockPathsCanContributeWhenPopulated) {
    // The fusion is path-agnostic: when F35/F38 land (D3.5) they just supply
    // their lists. Verify the mechanism by populating them here.
    FivePathInput in;
    in.contextualized = L({"a"});
    in.hype = L({"a"});
    auto out = FuseFivePathRrf(in);
    const RrfFusedHit* a = Find(out, "a");
    ASSERT_TRUE(a);
    EXPECT_TRUE(HasPath(*a, RrfPath::kContextualized));
    EXPECT_TRUE(HasPath(*a, RrfPath::kHypeQuestion));
}

// ---------- dedup / sort / truncate ----------

TEST(F40SparseRrfTest, DedupAcrossPaths) {
    FivePathInput in;
    in.dense = L({"a", "b"});
    in.sparse = L({"a", "b"});
    auto out = FuseFivePathRrf(in);
    EXPECT_EQ(out.size(), 2u);  // a, b deduped (not 4)
}

TEST(F40SparseRrfTest, TopNTruncates) {
    FivePathInput in;
    in.dense = L({"a", "b", "c", "d", "e"});
    auto out = FuseFivePathRrf(in, /*top_n=*/2);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].child_id, "a");
    EXPECT_EQ(out[1].child_id, "b");
}

TEST(F40SparseRrfTest, TopNZeroKeepsAll) {
    FivePathInput in;
    in.dense = L({"a", "b", "c"});
    EXPECT_EQ(FuseFivePathRrf(in, 0).size(), 3u);
}

TEST(F40SparseRrfTest, SortedDescendingWithDeterministicTieBreak) {
    FivePathInput in;
    // a and b only in dense at the same rank0 vs rank... give them equal scores:
    in.dense = L({"a"});
    in.sparse = L({"b"});  // both rank0 in their single path → equal 1/60
    auto out = FuseFivePathRrf(in);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_FLOAT_EQ(out[0].rrf_score, out[1].rrf_score);
    EXPECT_EQ(out[0].child_id, "a");  // tie broken by child_id asc
    EXPECT_EQ(out[1].child_id, "b");
}

TEST(F40SparseRrfTest, EmptyInputEmptyOutput) {
    FivePathInput in;
    EXPECT_TRUE(FuseFivePathRrf(in).empty());
}

TEST(F40SparseRrfTest, KZeroGuardedToDefault) {
    FivePathInput in;
    in.dense = L({"a"});
    auto out = FuseFivePathRrf(in, 0, /*k=*/0);  // k=0 → guarded to 60
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].rrf_score, 1.0f / 60.0f);  // not div-by-zero
}

}  // namespace
}  // namespace cortrix::retrieval
