#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <vector>

#include "cortrix/retrieval/sparse_retriever.h"

// Sparse retrieval S4 — ISparseRetriever interface contract. A header-only abstraction; this
// test pins the signatures + polymorphic contract by implementing a minimal
// in-memory retriever against it (the real SPLADE impl lands in S5/S6). It also
// doubles as the reusable mock other stories / D3.5 wiring can lean on.
namespace cortrix::retrieval {
namespace {

// Minimal in-memory ISparseRetriever — per-(ns, child) sparse vectors, brute
// dot-product Search. Exercises the contract, not the SPLADE inverted index.
class InMemorySparseRetriever : public ISparseRetriever {
public:
    std::vector<SparseHit> Search(const SparseVector& query,
                                  const NamespaceId& ns_id, int top_k) override {
        std::vector<SparseHit> hits;
        auto it = store_.find(ns_id);
        if (it == store_.end()) return hits;  // empty NS → empty (success)
        for (const auto& [child_id, vec] : it->second) {
            float score = 0.0f;
            for (const auto& [term, qw] : query.terms) {
                auto f = vec.terms.find(term);
                if (f != vec.terms.end()) score += qw * f->second;
            }
            if (score > 0.0f) hits.push_back({child_id, score});
        }
        std::sort(hits.begin(), hits.end(),
                  [](const SparseHit& a, const SparseHit& b) {
                      return a.score > b.score;
                  });
        if (top_k > 0 && static_cast<int>(hits.size()) > top_k) hits.resize(top_k);
        return hits;
    }

    Status Add(const NamespaceId& ns_id, const ChildId& child_id,
               const SparseVector& vec) override {
        if (vec.empty()) {
            return Remove(ns_id, child_id);  // dead chunk → drop (§6.5)
        }
        store_[ns_id][child_id] = vec;
        return Status::Ok();
    }

    Status Remove(const NamespaceId& ns_id, const ChildId& child_id) override {
        auto it = store_.find(ns_id);
        if (it != store_.end()) it->second.erase(child_id);  // idempotent
        return Status::Ok();
    }

    bool IsAvailable() const override { return available_; }
    void set_available(bool a) { available_ = a; }

private:
    std::map<NamespaceId, std::map<ChildId, SparseVector>> store_;
    bool available_ = true;
};

SparseVector V(std::map<uint32_t, float> t) {
    SparseVector v;
    v.terms = std::move(t);
    return v;
}

TEST(SparseRetrieverInterfaceTest, UsableViaBasePointer) {
    InMemorySparseRetriever impl;
    ISparseRetriever* r = &impl;  // polymorphic use
    EXPECT_TRUE(r->IsAvailable());
}

TEST(SparseRetrieverInterfaceTest, AddThenSearchReturnsHit) {
    InMemorySparseRetriever r;
    ASSERT_TRUE(r.Add("ns1", "child_a", V({{1, 0.9f}, {2, 0.5f}})).ok());
    auto hits = r.Search(V({{1, 1.0f}}), "ns1", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].child_id, "child_a");
    EXPECT_FLOAT_EQ(hits[0].score, 0.9f);
}

TEST(SparseRetrieverInterfaceTest, EmptyNamespaceReturnsEmpty) {
    InMemorySparseRetriever r;
    auto hits = r.Search(V({{1, 1.0f}}), "no_such_ns", 10);
    EXPECT_TRUE(hits.empty());  // empty NS = success, not error
}

TEST(SparseRetrieverInterfaceTest, SearchRanksByScoreAndTruncates) {
    InMemorySparseRetriever r;
    r.Add("ns", "low", V({{1, 0.1f}}));
    r.Add("ns", "high", V({{1, 0.9f}}));
    r.Add("ns", "mid", V({{1, 0.5f}}));
    auto hits = r.Search(V({{1, 1.0f}}), "ns", /*top_k=*/2);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].child_id, "high");
    EXPECT_EQ(hits[1].child_id, "mid");  // "low" dropped by top-K
}

TEST(SparseRetrieverInterfaceTest, AddEmptyVectorRemoves) {
    InMemorySparseRetriever r;
    r.Add("ns", "c", V({{1, 0.5f}}));
    ASSERT_EQ(r.Search(V({{1, 1.0f}}), "ns", 10).size(), 1u);
    ASSERT_TRUE(r.Add("ns", "c", V({})).ok());  // empty → remove (§6.5)
    EXPECT_TRUE(r.Search(V({{1, 1.0f}}), "ns", 10).empty());
}

TEST(SparseRetrieverInterfaceTest, RemoveIsIdempotent) {
    InMemorySparseRetriever r;
    EXPECT_TRUE(r.Remove("ns", "absent").ok());   // no-op success
    r.Add("ns", "c", V({{1, 0.5f}}));
    EXPECT_TRUE(r.Remove("ns", "c").ok());
    EXPECT_TRUE(r.Remove("ns", "c").ok());         // again → still ok
    EXPECT_TRUE(r.Search(V({{1, 1.0f}}), "ns", 10).empty());
}

TEST(SparseRetrieverInterfaceTest, ReAddReplacesPostings) {
    InMemorySparseRetriever r;
    r.Add("ns", "c", V({{1, 0.5f}}));
    r.Add("ns", "c", V({{2, 0.7f}}));  // replace, not merge
    EXPECT_TRUE(r.Search(V({{1, 1.0f}}), "ns", 10).empty());  // term 1 gone
    auto hits = r.Search(V({{2, 1.0f}}), "ns", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_FLOAT_EQ(hits[0].score, 0.7f);
}

TEST(SparseRetrieverInterfaceTest, NamespaceIsolation) {
    InMemorySparseRetriever r;
    r.Add("ns_a", "c", V({{1, 0.9f}}));
    r.Add("ns_b", "c", V({{1, 0.1f}}));
    auto a = r.Search(V({{1, 1.0f}}), "ns_a", 10);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_FLOAT_EQ(a[0].score, 0.9f);  // does not see ns_b's child
}

TEST(SparseRetrieverInterfaceTest, IsAvailableReflectsBackingState) {
    InMemorySparseRetriever r;
    EXPECT_TRUE(r.IsAvailable());
    r.set_available(false);
    EXPECT_FALSE(r.IsAvailable());  // query layer routes around (L2 fallback)
}

}  // namespace
}  // namespace cortrix::retrieval
