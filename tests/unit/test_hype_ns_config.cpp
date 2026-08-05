#include <gtest/gtest.h>

#include <string>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/hype_ns_config.h"

// HyPE S6 — K NS-config (default 3, NS-configurable 1-10) + IGlobalConfig
// default key. Parse / ResolveHypeK / ClampHypeK. Mirrors sparse retrieval NsSparseConfig.
namespace cortrix::spc {
namespace {

TEST(HypeNsConfigTest, BoundsConstants) {
    EXPECT_EQ(kHypeQuestionsDefault, 3);
    EXPECT_EQ(kHypeQuestionsMin, 1);
    EXPECT_EQ(kHypeQuestionsMax, 10);
}

// ---------- Parse ----------

TEST(HypeNsConfigTest, ParseEmptyInheritsGlobal) {
    auto r = NsHyPEConfig::Parse("");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST(HypeNsConfigTest, ParseEmptyObjectInheritsGlobal) {
    auto r = NsHyPEConfig::Parse("{}");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST(HypeNsConfigTest, ParseValidK) {
    auto r = NsHyPEConfig::Parse(R"({"questions_per_chunk": 5})");
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.value().questions_per_chunk.has_value());
    EXPECT_EQ(*r.value().questions_per_chunk, 5);
}

TEST(HypeNsConfigTest, ParseWrongTypeIgnored) {
    auto r = NsHyPEConfig::Parse(R"({"questions_per_chunk": "five"})");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().questions_per_chunk.has_value());  // degrades to global
}

TEST(HypeNsConfigTest, ParseUnknownKeysIgnored) {
    auto r = NsHyPEConfig::Parse(R"({"future": 1, "questions_per_chunk": 4})");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value().questions_per_chunk, 4);
}

TEST(HypeNsConfigTest, ParseNonObjectRejected) {
    auto arr = NsHyPEConfig::Parse("[1,2]");
    EXPECT_FALSE(arr.ok());
    EXPECT_NE(arr.status().message().find("CX_ERR_INVALID_CONFIG_JSON"),
              std::string::npos);
    EXPECT_FALSE(NsHyPEConfig::Parse("7").ok());
}

TEST(HypeNsConfigTest, ParseInvalidJsonRejected) {
    EXPECT_FALSE(NsHyPEConfig::Parse("{bad").ok());
}

TEST(HypeNsConfigTest, ParseNullInheritsGlobal) {
    auto r = NsHyPEConfig::Parse("null");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

// ---------- Clamp ----------

TEST(HypeNsConfigTest, ClampWithinRange) {
    EXPECT_EQ(ClampHypeK(3), 3);
    EXPECT_EQ(ClampHypeK(1), 1);
    EXPECT_EQ(ClampHypeK(10), 10);
}

TEST(HypeNsConfigTest, ClampBelowMin) {
    EXPECT_EQ(ClampHypeK(0), 1);
    EXPECT_EQ(ClampHypeK(-3), 1);
}

TEST(HypeNsConfigTest, ClampAboveMax) {
    EXPECT_EQ(ClampHypeK(11), 10);
    EXPECT_EQ(ClampHypeK(99), 10);
}

// ---------- Resolve ----------

TEST(HypeNsConfigTest, ResolveNsOverrideWins) {
    NsHyPEConfig ns;
    ns.questions_per_chunk = 7;
    InMemoryGlobalConfig g;
    g.Set(kHypeQuestionsPerChunkKey, "2");  // NS wins
    EXPECT_EQ(ResolveHypeK(ns, &g), 7);
}

TEST(HypeNsConfigTest, ResolveNsOverrideClamped) {
    NsHyPEConfig ns;
    ns.questions_per_chunk = 50;  // → clamp 10
    EXPECT_EQ(ResolveHypeK(ns, nullptr), 10);
    ns.questions_per_chunk = 0;   // → clamp 1
    EXPECT_EQ(ResolveHypeK(ns, nullptr), 1);
}

TEST(HypeNsConfigTest, ResolveFallsBackToGlobal) {
    NsHyPEConfig ns;  // no override
    InMemoryGlobalConfig g;
    g.Set(kHypeQuestionsPerChunkKey, "6");
    EXPECT_EQ(ResolveHypeK(ns, &g), 6);
}

TEST(HypeNsConfigTest, ResolveGlobalClamped) {
    NsHyPEConfig ns;
    InMemoryGlobalConfig g;
    g.Set(kHypeQuestionsPerChunkKey, "20");  // → clamp 10
    EXPECT_EQ(ResolveHypeK(ns, &g), 10);
}

TEST(HypeNsConfigTest, ResolveNoGlobalUsesCompileDefault) {
    NsHyPEConfig ns;
    EXPECT_EQ(ResolveHypeK(ns, nullptr), kHypeQuestionsDefault);  // 3
}

TEST(HypeNsConfigTest, ResolveMissingGlobalKeyUsesCompileDefault) {
    NsHyPEConfig ns;
    InMemoryGlobalConfig g;  // key not set
    EXPECT_EQ(ResolveHypeK(ns, &g), kHypeQuestionsDefault);
}

}  // namespace
}  // namespace cortrix::spc
