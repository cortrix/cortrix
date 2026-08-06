// S4.3 / S4.4 — request-level `rerank` boolean (RerankerRequestParams) + the
// three-layer priority resolution (request → NS → global). NS JSONB parse
// itself is covered in test_reranker_ns_config.cpp (S4.2).
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "cortrix/reranker/reranker_config.h"
#include "cortrix/reranker/reranker_config_resolver.h"
#include "cortrix/reranker/reranker_ns_config.h"

namespace cortrix::reranker {
namespace {

// ===================== S4.4 — three-layer resolution =====================

RerankerConfig GlobalDefaults() {
    RerankerConfig c;  // candidate_multiplier=3, max_candidates=50
    return c;
}

TEST(RerankerConfigResolverTest, AllAbsentUsesGlobal) {
    RerankerConfigResolver res(GlobalDefaults(), /*global_enabled=*/true);
    auto r = res.Resolve("{}");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().enabled);
    EXPECT_EQ(r.value().candidate_multiplier, 3);
    EXPECT_EQ(r.value().max_candidates, 50);
}

TEST(RerankerConfigResolverTest, NsOverridesMultiplierAndMax) {
    RerankerConfigResolver res(GlobalDefaults(), true);
    auto r = res.Resolve(R"({"candidate_multiplier": 5, "max_candidates": 100})");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().candidate_multiplier, 5);
    EXPECT_EQ(r.value().max_candidates, 100);
    EXPECT_TRUE(r.value().enabled);  // not overridden → global true
}

TEST(RerankerConfigResolverTest, NsDisablesReranker) {
    RerankerConfigResolver res(GlobalDefaults(), true);
    auto r = res.Resolve(R"({"enabled": false})");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().enabled);
}

// --- worked example: NS enabled=false, request rerank=true → enabled ---
TEST(RerankerConfigResolverTest, RequestTrueBeatsNsFalse) {
    RerankerConfigResolver res(GlobalDefaults(), true);
    RerankerRequestParams req;
    req.rerank = true;
    auto r = res.Resolve(R"({"enabled": false})", req);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().enabled);  // request wins
}

// --- request rerank=false beats NS enabled=true + global true ---
TEST(RerankerConfigResolverTest, RequestFalseBeatsNsTrue) {
    RerankerConfigResolver res(GlobalDefaults(), true);
    RerankerRequestParams req;
    req.rerank = false;
    auto r = res.Resolve(R"({"enabled": true})", req);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().enabled);  // request wins
}

// --- NS enabled=false beats global true when request absent ---
TEST(RerankerConfigResolverTest, NsFalseBeatsGlobalTrueWhenNoRequest) {
    RerankerConfigResolver res(GlobalDefaults(), true);
    auto r = res.Resolve(R"({"enabled": false})");  // no request param
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().enabled);
}

// --- The full enabled priority matrix (request × NS × global) ---
TEST(RerankerConfigResolverTest, EnabledPriorityMatrix) {
    struct Case {
        std::optional<bool> request;
        std::optional<bool> ns;
        bool global;
        bool expected;
    };
    const Case cases[] = {
        // request set → request wins regardless of NS/global
        {true,  false, false, true},
        {false, true,  true,  false},
        {true,  true,  false, true},
        {false, false, true,  false},
        // request absent, NS set → NS wins over global
        {std::nullopt, true,  false, true},
        {std::nullopt, false, true,  false},
        // request + NS absent → global
        {std::nullopt, std::nullopt, true,  true},
        {std::nullopt, std::nullopt, false, false},
    };
    for (const auto& tc : cases) {
        RerankerConfigResolver res(GlobalDefaults(), tc.global);
        NsRerankerConfig ns;
        ns.enabled = tc.ns;
        RerankerRequestParams req;
        req.rerank = tc.request;
        EXPECT_EQ(res.Resolve(ns, req).enabled, tc.expected)
            << "req=" << (tc.request ? (*tc.request ? "T" : "F") : "-")
            << " ns=" << (tc.ns ? (*tc.ns ? "T" : "F") : "-")
            << " global=" << tc.global;
    }
}

TEST(RerankerConfigResolverTest, ResolveErrorOnBadNsJson) {
    RerankerConfigResolver res(GlobalDefaults(), true);
    EXPECT_FALSE(res.Resolve("[]").ok());
}

// ===================== top_N computation (E2) =====================

TEST(EffectiveRerankerConfigTest, ComputeTopNAppliesMultiplierAndCap) {
    EffectiveRerankerConfig eff{true, 3, 50};
    EXPECT_EQ(eff.ComputeTopN(10), 30);   // 10×3 = 30 ≤ 50
    EXPECT_EQ(eff.ComputeTopN(20), 50);   // 20×3 = 60, capped at 50
}

TEST(EffectiveRerankerConfigTest, ComputeTopNFloorsAtTopK) {
    EffectiveRerankerConfig eff{true, 1, 5};
    EXPECT_EQ(eff.ComputeTopN(10), 10);   // cap 5 < top_k 10 → floor at top_k
}

TEST(EffectiveRerankerConfigTest, ComputeTopNZeroForNonPositiveTopK) {
    EffectiveRerankerConfig eff{true, 3, 50};
    EXPECT_EQ(eff.ComputeTopN(0), 0);
    EXPECT_EQ(eff.ComputeTopN(-1), 0);
}

}  // namespace
}  // namespace cortrix::reranker
