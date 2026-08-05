// S4.2 — NS-level reranker_config JSONB parse (reranker / Issue 2.2): the 3
// B-class fields (enabled / candidate_multiplier / max_candidates), every field
// optional, unknown keys ignored, malformed individual values degrade to unset
// (never crash the query path), non-object / invalid JSON → CX_ERR.
#include <gtest/gtest.h>

#include <string>

#include "cortrix/reranker/reranker_ns_config.h"

namespace cortrix::reranker {
namespace {

TEST(NsRerankerConfigTest, EmptyBlobInheritsGlobal) {
    auto r = NsRerankerConfig::Parse("");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST(NsRerankerConfigTest, EmptyObjectInheritsGlobal) {
    auto r = NsRerankerConfig::Parse("{}");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST(NsRerankerConfigTest, ParsesAllThreeFields) {
    auto r = NsRerankerConfig::Parse(
        R"({"enabled": false, "candidate_multiplier": 5, "max_candidates": 100})");
    ASSERT_TRUE(r.ok());
    const NsRerankerConfig& c = r.value();
    ASSERT_TRUE(c.enabled.has_value());
    EXPECT_FALSE(*c.enabled);
    ASSERT_TRUE(c.candidate_multiplier.has_value());
    EXPECT_EQ(*c.candidate_multiplier, 5);
    ASSERT_TRUE(c.max_candidates.has_value());
    EXPECT_EQ(*c.max_candidates, 100);
    EXPECT_FALSE(c.empty());
}

TEST(NsRerankerConfigTest, PartialFieldsLeaveOthersUnset) {
    auto r = NsRerankerConfig::Parse(R"({"candidate_multiplier": 2})");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().enabled.has_value());
    EXPECT_EQ(*r.value().candidate_multiplier, 2);
    EXPECT_FALSE(r.value().max_candidates.has_value());
}

TEST(NsRerankerConfigTest, UnknownKeysIgnored) {
    // Phase-2 keys (model / score_threshold) + arbitrary keys → ignored, no error.
    auto r = NsRerankerConfig::Parse(
        R"({"enabled": true, "model": "zerank-1-small", "score_threshold": 0.5, "foo": 1})");
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.value().enabled.has_value());
    EXPECT_TRUE(*r.value().enabled);
}

TEST(NsRerankerConfigTest, WrongTypedValueIgnoredNotError) {
    // enabled as a string / multiplier as a float → ignored (degrade to global),
    // must NOT fail the query path.
    auto r = NsRerankerConfig::Parse(
        R"({"enabled": "yes", "candidate_multiplier": 3.5, "max_candidates": 50})");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().enabled.has_value());               // "yes" ignored
    EXPECT_FALSE(r.value().candidate_multiplier.has_value());  // 3.5 ignored
    EXPECT_EQ(*r.value().max_candidates, 50);                  // valid int kept
}

TEST(NsRerankerConfigTest, NonObjectJsonIsError) {
    EXPECT_FALSE(NsRerankerConfig::Parse("[]").ok());
    EXPECT_FALSE(NsRerankerConfig::Parse("5").ok());
    Status s = NsRerankerConfig::Parse("[1,2]").status();
    EXPECT_NE(s.message().find("CX_ERR_INVALID_CONFIG_JSON"), std::string::npos);
}

TEST(NsRerankerConfigTest, MalformedJsonIsError) {
    Status s = NsRerankerConfig::Parse("{not json").status();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_INVALID_CONFIG_JSON"), std::string::npos);
}

TEST(NsRerankerConfigTest, NullBlobInheritsGlobal) {
    auto r = NsRerankerConfig::Parse("null");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

}  // namespace
}  // namespace cortrix::reranker
