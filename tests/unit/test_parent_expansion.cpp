#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "cortrix/query/parent_expansion.h"
#include "cortrix/retrieval/cross_ns_types.h"

// Parent expansion resolved against the FINAL response (F34 §2.5 / F41 §8.1).
//
// The defect these tests exist for: expansion used to be decided inside a namespace,
// before RAG-fusion re-ordering, the LLM listwise rerank and its trim, and CRAG's
// halving. A hit promoted by the rerank therefore arrived first with no parent text
// while a demoted hit carried it, and no later pass could repair that — the budget
// pass can only clear text, never fetch it.
namespace cortrix::query {
namespace {

retrieval::ResultItem Item(const std::string& child, const std::string& ns,
                           const std::string& parent) {
    retrieval::ResultItem it;
    it.child_id = child;
    it.parent_id = parent;
    it.namespace_id = ns;
    it.content = "chunk text of " + child;
    return it;
}

CortrixParent Parent(const std::string& id, std::size_t size, int64_t byte_start) {
    CortrixParent p;
    p.parent_id = id;
    p.parent_text = std::string(size, 'x');
    p.token_count = static_cast<int64_t>(size / 4);
    p.byte_offset_start = byte_start;
    p.byte_offset_end = byte_start + static_cast<int64_t>(size);
    return p;
}

// A fetch callback that records what it was asked for, so "one call per namespace,
// distinct ids only" is observed rather than assumed.
struct RecordingFetch {
    std::vector<CortrixParent> rows;
    mutable std::vector<std::string> namespaces_called;
    mutable std::vector<std::vector<std::string>> ids_called;

    std::vector<CortrixParent> operator()(const std::string& ns,
                                          const std::vector<std::string>& ids) const {
        namespaces_called.push_back(ns);
        ids_called.push_back(ids);
        std::vector<CortrixParent> out;
        for (const auto& row : rows) {
            for (const auto& id : ids) {
                if (row.parent_id == id) out.push_back(row);
            }
        }
        return out;
    }
};

// --- Step 1: selection reads the final order -----------------------------------

// The regression in one assertion: the SAME two hits, ordered the two ways the
// listwise rerank can return them, select different results for expansion. Deciding
// inside a namespace cannot produce this, which is exactly why it was wrong.
TEST(SelectParentExpansionTargetsTest, FollowsTheFinalOrderNotAnEarlierOne) {
    CrossNsResponse pre_swap;
    pre_swap.results.push_back(Item("c1", "ns_a", "p1"));
    pre_swap.results.push_back(Item("c2", "ns_a", "p2"));

    CrossNsResponse post_swap;  // what the listwise rerank's [2,1] produces
    post_swap.results.push_back(Item("c2", "ns_a", "p2"));
    post_swap.results.push_back(Item("c1", "ns_a", "p1"));

    const auto before = SelectParentExpansionTargets(pre_swap, /*expand_top_k=*/1);
    const auto after = SelectParentExpansionTargets(post_swap, /*expand_top_k=*/1);

    ASSERT_EQ(before.size(), 1u);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(before[0].parent_id, "p1");
    EXPECT_EQ(after[0].parent_id, "p2")
        << "the promoted hit must be the one that gets expanded";
}

// Depth 0 is the default, and it is a strict no-op: nothing to fetch, so an opted-out
// response never pays a lookup.
TEST(SelectParentExpansionTargetsTest, ZeroAndNegativeDepthSelectNothing) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    EXPECT_TRUE(SelectParentExpansionTargets(resp, 0).empty());
    EXPECT_TRUE(SelectParentExpansionTargets(resp, -3).empty());
}

// A result with nothing to look up must not consume a slot. Letting it would shrink
// the window the caller asked for, silently and only on some corpora.
TEST(SelectParentExpansionTargetsTest, ResultsWithoutAParentDoNotConsumeDepth) {
    CrossNsResponse resp;
    resp.results.push_back(Item("doc-level", "ns_a", ""));  // doc pseudo-result
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    resp.results.push_back(Item("c2", "ns_a", "p2"));

    const auto targets = SelectParentExpansionTargets(resp, /*expand_top_k=*/2);

    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].parent_id, "p1");
    EXPECT_EQ(targets[0].result_index, 1u);
    EXPECT_EQ(targets[1].parent_id, "p2");
    EXPECT_EQ(targets[1].result_index, 2u);
}

TEST(SelectParentExpansionTargetsTest, DepthLargerThanTheResultSetIsClamped) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    EXPECT_EQ(SelectParentExpansionTargets(resp, 99).size(), 1u);
}

TEST(SelectParentExpansionTargetsTest, EmptyResponseIsANoOp) {
    CrossNsResponse resp;
    EXPECT_TRUE(SelectParentExpansionTargets(resp, 4).empty());
}

// --- Step 2: fetching, grouped by namespace -------------------------------------

// Two hits sharing a parent cost ONE lookup. (They still cost two copies on the wire,
// which is why step 3 charges the budget per result — see ByteBudgetIsChargedPerResult.)
TEST(FetchParentsForTargetsTest, OneLookupPerDistinctIdPerNamespace) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    resp.results.push_back(Item("c2", "ns_a", "p1"));
    resp.results.push_back(Item("c3", "ns_a", "p2"));
    const auto targets = SelectParentExpansionTargets(resp, 3);

    RecordingFetch fetch;
    fetch.rows = {Parent("p1", 10, 0), Parent("p2", 10, 10)};
    const ParentTextByKey parents = FetchParentsForTargets(targets, std::ref(fetch));

    ASSERT_EQ(fetch.namespaces_called.size(), 1u) << "one acquire for one namespace";
    EXPECT_EQ(fetch.ids_called[0], (std::vector<std::string>{"p1", "p2"}));
    EXPECT_EQ(parents.size(), 2u);
}

// The key is (namespace, parent_id), not parent_id alone: ids are unique within a
// namespace's store, not across the response. Keying on the id alone would serve one
// namespace's parent text as another's.
TEST(FetchParentsForTargetsTest, SameParentIdInTwoNamespacesStaysDistinct) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "shared-id"));
    resp.results.push_back(Item("c2", "ns_b", "shared-id"));
    const auto targets = SelectParentExpansionTargets(resp, 2);

    const ParentTextByKey parents = FetchParentsForTargets(
        targets, [](const std::string& ns, const std::vector<std::string>& ids) {
            std::vector<CortrixParent> out;
            CortrixParent p = Parent(ids.front(), 4, 0);
            p.parent_text = ns == "ns_a" ? "AAAA" : "BBBB";
            out.push_back(p);
            return out;
        });

    ASSERT_EQ(parents.size(), 2u);
    EXPECT_EQ(parents.at({"ns_a", "shared-id"}).parent_text, "AAAA");
    EXPECT_EQ(parents.at({"ns_b", "shared-id"}).parent_text, "BBBB");
}

// An unresolvable namespace returns nothing and the other namespace still resolves:
// one bad namespace must not cost the whole response its parent text.
TEST(FetchParentsForTargetsTest, UnresolvableNamespaceDegradesOnlyItself) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_down", "p1"));
    resp.results.push_back(Item("c2", "ns_up", "p2"));
    const auto targets = SelectParentExpansionTargets(resp, 2);

    const ParentTextByKey parents = FetchParentsForTargets(
        targets, [](const std::string& ns, const std::vector<std::string>& ids) {
            std::vector<CortrixParent> out;
            if (ns == "ns_down") return out;  // acquire failed
            out.push_back(Parent(ids.front(), 10, 0));
            return out;
        });

    ASSERT_EQ(parents.size(), 1u);
    EXPECT_EQ(parents.count({"ns_up", "p2"}), 1u);
}

TEST(FetchParentsForTargetsTest, NoTargetsAndNoCallbackAreNoOps) {
    EXPECT_TRUE(FetchParentsForTargets({}, [](const std::string&,
                                              const std::vector<std::string>&) {
                    return std::vector<CortrixParent>{};
                }).empty());
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    EXPECT_TRUE(FetchParentsForTargets(SelectParentExpansionTargets(resp, 1), nullptr).empty());
}

// --- Step 3: applying under one response-wide budget ----------------------------

TEST(ApplyParentExpansionTest, WritesTextAndTheSpanMetadata) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    const auto targets = SelectParentExpansionTargets(resp, 1);
    ParentTextByKey parents;
    parents.emplace(std::make_pair(std::string("ns_a"), std::string("p1")),
                    Parent("p1", 40, 100));

    EXPECT_EQ(ApplyParentExpansion(resp, targets, parents, /*max_bytes=*/0), 1);

    EXPECT_EQ(resp.results[0].parent_content.size(), 40u);
    EXPECT_EQ(resp.results[0].metadata.at("parent_token_count"), "10");
    EXPECT_EQ(resp.results[0].metadata.at("parent_byte_start"), "100");
    EXPECT_EQ(resp.results[0].metadata.at("parent_byte_end"), "140");
}

// One budget for the whole response, not one per namespace: two namespaces each
// contributing an 80-byte parent must not spend 160 against a 100-byte cap.
TEST(ApplyParentExpansionTest, ByteBudgetSpansNamespaces) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    resp.results.push_back(Item("c2", "ns_b", "p2"));
    const auto targets = SelectParentExpansionTargets(resp, 2);
    ParentTextByKey parents;
    parents.emplace(std::make_pair(std::string("ns_a"), std::string("p1")), Parent("p1", 80, 0));
    parents.emplace(std::make_pair(std::string("ns_b"), std::string("p2")), Parent("p2", 80, 0));

    EXPECT_EQ(ApplyParentExpansion(resp, targets, parents, /*max_bytes=*/100), 1);

    EXPECT_EQ(resp.results[0].parent_content.size(), 80u);
    EXPECT_TRUE(resp.results[1].parent_content.empty());
}

// The budget is charged per RESULT even when the lookup was shared, because each
// result carries its own copy of the text on the wire.
TEST(ApplyParentExpansionTest, ByteBudgetIsChargedPerResult) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    resp.results.push_back(Item("c2", "ns_a", "p1"));  // same parent
    const auto targets = SelectParentExpansionTargets(resp, 2);
    ParentTextByKey parents;
    parents.emplace(std::make_pair(std::string("ns_a"), std::string("p1")), Parent("p1", 80, 0));

    EXPECT_EQ(ApplyParentExpansion(resp, targets, parents, /*max_bytes=*/100), 1);

    EXPECT_EQ(resp.results[0].parent_content.size(), 80u);
    EXPECT_TRUE(resp.results[1].parent_content.empty());
}

// Prefix semantics: the first parent that does not fit stops the pass. Skipping it to
// admit a smaller, lower-ranked parent would leave the caller's higher-ranked result
// without what a lower one has — the shape of the defect this file fixes.
TEST(ApplyParentExpansionTest, ABudgetMissStopsRatherThanSkipping) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "big"));
    resp.results.push_back(Item("c2", "ns_a", "small"));
    const auto targets = SelectParentExpansionTargets(resp, 2);
    ParentTextByKey parents;
    parents.emplace(std::make_pair(std::string("ns_a"), std::string("big")), Parent("big", 90, 0));
    parents.emplace(std::make_pair(std::string("ns_a"), std::string("small")),
                    Parent("small", 5, 0));

    EXPECT_EQ(ApplyParentExpansion(resp, targets, parents, /*max_bytes=*/10), 0);

    EXPECT_TRUE(resp.results[0].parent_content.empty());
    EXPECT_TRUE(resp.results[1].parent_content.empty())
        << "a lower-ranked result must not carry parent text the top one lacks";
}

// A dangling link is NOT a budget event: it degrades that one result and the pass
// continues, so one broken row cannot strip the whole response.
TEST(ApplyParentExpansionTest, MissingParentIsSkippedNotFatalAndDoesNotStop) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "dangling"));
    resp.results.push_back(Item("c2", "ns_a", "p2"));
    const auto targets = SelectParentExpansionTargets(resp, 2);
    ParentTextByKey parents;
    parents.emplace(std::make_pair(std::string("ns_a"), std::string("p2")), Parent("p2", 20, 0));

    EXPECT_EQ(ApplyParentExpansion(resp, targets, parents, /*max_bytes=*/0), 1);

    EXPECT_TRUE(resp.results[0].parent_content.empty());
    EXPECT_EQ(resp.results[0].metadata.count("parent_byte_start"), 0u)
        << "no span metadata without the span";
    EXPECT_EQ(resp.results[1].parent_content.size(), 20u);
}

TEST(ApplyParentExpansionTest, NoTargetsLeavesTheResponseUntouched) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    EXPECT_EQ(ApplyParentExpansion(resp, {}, {}, 1024), 0);
    EXPECT_TRUE(resp.results[0].parent_content.empty());
    EXPECT_TRUE(resp.results[0].metadata.empty());
}

// --- The three steps together ---------------------------------------------------

// End of the chain, with the ranking swap the listwise rerank actually performs:
// two stored parents, the order reversed, depth 1. The result the caller sees FIRST
// is the one carrying parent text and span metadata; the other carries neither.
TEST(ParentExpansionPipelineTest, RankingSwapMovesTheExpansionToThePromotedHit) {
    const auto run = [](bool swapped) {
        CrossNsResponse resp;
        if (swapped) {
            resp.results.push_back(Item("c2", "ns_a", "p2"));
            resp.results.push_back(Item("c1", "ns_a", "p1"));
        } else {
            resp.results.push_back(Item("c1", "ns_a", "p1"));
            resp.results.push_back(Item("c2", "ns_a", "p2"));
        }
        const auto targets = SelectParentExpansionTargets(resp, /*expand_top_k=*/1);
        const ParentTextByKey parents = FetchParentsForTargets(
            targets, [](const std::string&, const std::vector<std::string>& ids) {
                std::vector<CortrixParent> out;
                for (const auto& id : ids) {
                    CortrixParent p = Parent(id, 32, 0);
                    p.parent_text = std::string(32, id == "p1" ? 'A' : 'B');
                    out.push_back(p);
                }
                return out;
            });
        EXPECT_EQ(ApplyParentExpansion(resp, targets, parents, /*max_bytes=*/1024), 1);
        return resp;
    };

    const CrossNsResponse before = run(/*swapped=*/false);
    EXPECT_EQ(before.results[0].parent_content, std::string(32, 'A'));
    EXPECT_TRUE(before.results[1].parent_content.empty());

    const CrossNsResponse after = run(/*swapped=*/true);
    EXPECT_EQ(after.results[0].child_id, "c2");
    EXPECT_EQ(after.results[0].parent_content, std::string(32, 'B'))
        << "the hit the rerank promoted is the one that must carry parent text";
    EXPECT_EQ(after.results[0].metadata.at("parent_token_count"), "8");
    EXPECT_TRUE(after.results[1].parent_content.empty());
    EXPECT_EQ(after.results[1].metadata.count("parent_token_count"), 0u);
}

// Depth and byte bound applied together, both honoured, in final order.
TEST(ParentExpansionPipelineTest, DepthAndByteBoundHoldTogether) {
    CrossNsResponse resp;
    resp.results.push_back(Item("c1", "ns_a", "p1"));
    resp.results.push_back(Item("c2", "ns_b", "p2"));
    resp.results.push_back(Item("c3", "ns_a", "p3"));

    const auto targets = SelectParentExpansionTargets(resp, /*expand_top_k=*/2);
    ASSERT_EQ(targets.size(), 2u) << "depth bounds how many are even looked up";

    const ParentTextByKey parents = FetchParentsForTargets(
        targets, [](const std::string&, const std::vector<std::string>& ids) {
            std::vector<CortrixParent> out;
            for (const auto& id : ids) out.push_back(Parent(id, 60, 0));
            return out;
        });

    EXPECT_EQ(ApplyParentExpansion(resp, targets, parents, /*max_bytes=*/100), 1);
    EXPECT_EQ(resp.results[0].parent_content.size(), 60u);
    EXPECT_TRUE(resp.results[1].parent_content.empty()) << "60 + 60 exceeds 100";
    EXPECT_TRUE(resp.results[2].parent_content.empty()) << "outside the requested depth";
}

}  // namespace
}  // namespace cortrix::query
