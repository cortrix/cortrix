#include "cortrix/spc_enricher/enricher_config_resolver.h"

#include <gtest/gtest.h>

#include <string>

#include "cortrix/spc_enricher.h"

namespace cortrix::spc {
namespace {

EnricherConfig Global() {
    EnricherConfig g;
    g.enabled = true;
    g.model = "gpt-4o-mini";
    g.score_threshold = 0.0f;
    g.prompt_template_id = "default-zh";
    return g;
}

// --- EnricherNsConfig::Parse -------------------------------------------------

TEST(EnricherNsConfigTest, EmptyAndBraceInheritGlobal) {
    auto a = EnricherNsConfig::Parse("");
    ASSERT_TRUE(a.ok());
    EXPECT_TRUE(a.value().empty());
    auto b = EnricherNsConfig::Parse("{}");
    ASSERT_TRUE(b.ok());
    EXPECT_TRUE(b.value().empty());
}

TEST(EnricherNsConfigTest, ParsesFourBClassKeys) {
    auto r = EnricherNsConfig::Parse(
        R"({"enabled":false,"model":"gpt-4o","score_threshold":0.7,"prompt_template_id":"default-en"})");
    ASSERT_TRUE(r.ok());
    const auto& ns = r.value();
    ASSERT_TRUE(ns.enabled.has_value());
    EXPECT_FALSE(*ns.enabled);
    EXPECT_EQ(*ns.model, "gpt-4o");
    EXPECT_FLOAT_EQ(*ns.score_threshold, 0.7f);
    EXPECT_EQ(*ns.prompt_template_id, "default-en");
}

TEST(EnricherNsConfigTest, WrongTypeValuesIgnoredDefensively) {
    // enabled as string, score as string → ignored (degrade to global), not error.
    auto r = EnricherNsConfig::Parse(R"({"enabled":"yes","score_threshold":"hi","model":42})");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());  // none of the malformed values took effect
}

TEST(EnricherNsConfigTest, UnknownKeysIgnored) {
    auto r = EnricherNsConfig::Parse(R"({"future_key":123,"model":"gpt-4o"})");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value().model, "gpt-4o");
}

TEST(EnricherNsConfigTest, NonObjectJsonIsError) {
    EXPECT_FALSE(EnricherNsConfig::Parse("[1,2,3]").ok());
    EXPECT_FALSE(EnricherNsConfig::Parse("5").ok());
    EXPECT_FALSE(EnricherNsConfig::Parse("not json").ok());
}

TEST(EnricherNsConfigTest, JsonNullInheritsGlobal) {
    auto r = EnricherNsConfig::Parse("null");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

// --- EnricherConfigResolver (three-layer §2.9) -------------------------------

TEST(EnricherConfigResolverTest, EmptyNsInheritsGlobal) {
    EnricherConfigResolver r(Global());
    auto eff = r.Resolve(std::string("{}"));
    ASSERT_TRUE(eff.ok());
    EXPECT_TRUE(eff.value().enabled);
    EXPECT_EQ(eff.value().model, "gpt-4o-mini");
    EXPECT_EQ(eff.value().prompt_template_id, "default-zh");
}

TEST(EnricherConfigResolverTest, NsOverridesGlobalBClass) {
    EnricherConfigResolver r(Global());
    auto eff = r.Resolve(std::string(
        R"({"model":"gpt-4o","score_threshold":0.5,"prompt_template_id":"default-en"})"));
    ASSERT_TRUE(eff.ok());
    EXPECT_EQ(eff.value().model, "gpt-4o");
    EXPECT_FLOAT_EQ(eff.value().score_threshold, 0.5f);
    EXPECT_EQ(eff.value().prompt_template_id, "default-en");
    EXPECT_TRUE(eff.value().enabled);  // not overridden → global
}

TEST(EnricherConfigResolverTest, RequestEnrichIsHighestPriority) {
    // §2.9 example: NS {"enabled":false} + request enrich=true → enabled.
    EnricherConfigResolver r(Global());
    EnricherRequestParams req;
    req.enrich = true;
    auto eff = r.Resolve(std::string(R"({"enabled":false})"), req);
    ASSERT_TRUE(eff.ok());
    EXPECT_TRUE(eff.value().enabled);  // request wins over NS false
}

TEST(EnricherConfigResolverTest, RequestEnrichFalseDisables) {
    EnricherConfigResolver r(Global());
    EnricherRequestParams req;
    req.enrich = false;
    auto eff = r.Resolve(std::string(R"({"enabled":true})"), req);
    ASSERT_TRUE(eff.ok());
    EXPECT_FALSE(eff.value().enabled);
}

TEST(EnricherConfigResolverTest, AClassFieldsAlwaysFromGlobal) {
    // A-class GUCs are not NS-overridable — even if present in the blob (unknown
    // keys), they come from global.
    EnricherConfig g = Global();
    g.endpoint = "https://global.example/v1";
    g.workers = 7;
    g.budget_cap_usd = 99;
    EnricherConfigResolver r(g);
    auto eff = r.Resolve(std::string(R"({"endpoint":"https://evil","workers":1,"model":"gpt-4o"})"));
    ASSERT_TRUE(eff.ok());
    EXPECT_EQ(eff.value().endpoint, "https://global.example/v1");  // ignored NS attempt
    EXPECT_EQ(eff.value().workers, 7);
    EXPECT_EQ(eff.value().budget_cap_usd, 99);
    EXPECT_EQ(eff.value().model, "gpt-4o");  // B-class did override
}

TEST(EnricherConfigResolverTest, MalformedNsBlobIsError) {
    EnricherConfigResolver r(Global());
    EXPECT_FALSE(r.Resolve(std::string("[1,2]")).ok());
}

}  // namespace
}  // namespace cortrix::spc
