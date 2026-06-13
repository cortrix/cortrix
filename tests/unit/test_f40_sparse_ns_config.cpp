#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/retrieval/sparse_ns_config.h"
#include "cortrix/retrieval/splade_sparse_retriever.h"

// F40 S7 — top-K NS-config (F40-6: default 100, NS-configurable 50-200) + the
// IGlobalConfig default key. Parse() / ResolveSparseTopK() / ClampSparseTopK()
// + an end-to-end check that the resolved value drives Search.
namespace cortrix::retrieval {
namespace {

// ---------- bounds constants ----------

TEST(F40SparseNsConfigTest, BoundsConstants) {
    EXPECT_EQ(kSparseTopKDefault, 100);
    EXPECT_EQ(kSparseTopKMin, 50);
    EXPECT_EQ(kSparseTopKMax, 200);
}

// ---------- Parse ----------

TEST(F40SparseNsConfigTest, ParseEmptyInheritsGlobal) {
    auto r = NsSparseConfig::Parse("");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST(F40SparseNsConfigTest, ParseEmptyObjectInheritsGlobal) {
    auto r = NsSparseConfig::Parse("{}");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST(F40SparseNsConfigTest, ParseValidTopK) {
    auto r = NsSparseConfig::Parse(R"({"sparse_top_k": 150})");
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.value().sparse_top_k.has_value());
    EXPECT_EQ(*r.value().sparse_top_k, 150);
}

TEST(F40SparseNsConfigTest, ParseWrongTypeIgnored) {
    // A bad value type must not crash the query path — it degrades to global.
    auto r = NsSparseConfig::Parse(R"({"sparse_top_k": "abc"})");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().sparse_top_k.has_value());
}

TEST(F40SparseNsConfigTest, ParseUnknownKeysIgnored) {
    auto r = NsSparseConfig::Parse(R"({"future_key": 5, "sparse_top_k": 80})");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value().sparse_top_k, 80);
}

TEST(F40SparseNsConfigTest, ParseNonObjectRejected) {
    auto arr = NsSparseConfig::Parse("[1,2,3]");
    EXPECT_FALSE(arr.ok());
    EXPECT_NE(arr.status().message().find("CX_ERR_INVALID_CONFIG_JSON"),
              std::string::npos);
    auto scalar = NsSparseConfig::Parse("5");
    EXPECT_FALSE(scalar.ok());
}

TEST(F40SparseNsConfigTest, ParseInvalidJsonRejected) {
    auto r = NsSparseConfig::Parse("{not json");
    EXPECT_FALSE(r.ok());
}

TEST(F40SparseNsConfigTest, ParseNullInheritsGlobal) {
    auto r = NsSparseConfig::Parse("null");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

// ---------- Clamp ----------

TEST(F40SparseNsConfigTest, ClampWithinRangeUnchanged) {
    EXPECT_EQ(ClampSparseTopK(100), 100);
    EXPECT_EQ(ClampSparseTopK(50), 50);
    EXPECT_EQ(ClampSparseTopK(200), 200);
}

TEST(F40SparseNsConfigTest, ClampBelowMin) {
    EXPECT_EQ(ClampSparseTopK(10), 50);
    EXPECT_EQ(ClampSparseTopK(0), 50);
    EXPECT_EQ(ClampSparseTopK(-5), 50);
}

TEST(F40SparseNsConfigTest, ClampAboveMax) {
    EXPECT_EQ(ClampSparseTopK(500), 200);
    EXPECT_EQ(ClampSparseTopK(201), 200);
}

// ---------- Resolve ----------

TEST(F40SparseNsConfigTest, ResolveNsOverrideWins) {
    NsSparseConfig ns;
    ns.sparse_top_k = 150;
    InMemoryGlobalConfig g;
    g.Set(kSparseTopKConfigKey, "75");  // NS wins over this
    EXPECT_EQ(ResolveSparseTopK(ns, &g), 150);
}

TEST(F40SparseNsConfigTest, ResolveNsOverrideClamped) {
    NsSparseConfig ns;
    ns.sparse_top_k = 9999;  // out of range → clamp to 200
    EXPECT_EQ(ResolveSparseTopK(ns, nullptr), 200);
    ns.sparse_top_k = 1;     // → clamp to 50
    EXPECT_EQ(ResolveSparseTopK(ns, nullptr), 50);
}

TEST(F40SparseNsConfigTest, ResolveFallsBackToGlobal) {
    NsSparseConfig ns;  // no override
    InMemoryGlobalConfig g;
    g.Set(kSparseTopKConfigKey, "175");
    EXPECT_EQ(ResolveSparseTopK(ns, &g), 175);
}

TEST(F40SparseNsConfigTest, ResolveGlobalClamped) {
    NsSparseConfig ns;
    InMemoryGlobalConfig g;
    g.Set(kSparseTopKConfigKey, "300");  // out of range → 200
    EXPECT_EQ(ResolveSparseTopK(ns, &g), 200);
}

TEST(F40SparseNsConfigTest, ResolveNoGlobalUsesCompileDefault) {
    NsSparseConfig ns;  // no override
    EXPECT_EQ(ResolveSparseTopK(ns, nullptr), kSparseTopKDefault);  // 100
}

TEST(F40SparseNsConfigTest, ResolveMissingGlobalKeyUsesCompileDefault) {
    NsSparseConfig ns;
    InMemoryGlobalConfig g;  // key not set
    EXPECT_EQ(ResolveSparseTopK(ns, &g), kSparseTopKDefault);
}

// ---------- end-to-end: resolved top-K drives Search ----------

TEST(F40SparseNsConfigTest, ResolvedTopKDrivesSearch) {
    SparseVector q;
    q.terms = {{1, 1.0f}};
    SpladeSparseRetriever r(SpladeConfig{}, ":memory:");
    ASSERT_TRUE(r.Open().ok());
    for (int i = 0; i < 80; ++i) {
        SparseVector v;
        v.terms = {{1, 0.01f * (i + 1)}};
        ASSERT_TRUE(r.Add("ns", "c" + std::to_string(i), v).ok());
    }
    // NS override 60 → Search returns 60 (clamped within [50,200]).
    NsSparseConfig ns;
    ns.sparse_top_k = 60;
    int k = ResolveSparseTopK(ns, nullptr);
    EXPECT_EQ(k, 60);
    auto hits = r.Search(q, "ns", k);
    EXPECT_EQ(hits.size(), 60u);
}

}  // namespace
}  // namespace cortrix::retrieval
