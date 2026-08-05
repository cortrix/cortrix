#include <gtest/gtest.h>

#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/config_resolver.h"
#include "cortrix/common/status.h"

// S3.2 coverage: ConfigResolver<T> three-layer override (global ← NS ← request)
// with a request whitelist. Uses a sample JSON-serializable config.
//
// SampleConfig + its to_json/from_json (macro) + operator== live in a NAMED
// namespace (not the anonymous one below) on purpose: the macro emits free
// functions reached only via ADL/templates, which some compilers (gcc
// -Wunused-function) flag as unused if they sit in an anonymous namespace.
// Namespace-scope (non-static) functions are never flagged.
namespace cortrix::catalog::config_resolver_test {

// A stand-in for a Feature config struct (e.g. RerankerConfig). nlohmann macro
// gives it to_json/from_json so the resolver can merge at the JSON layer.
struct SampleConfig {
    bool enabled = false;
    int top_k = 10;
    std::string model = "default";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SampleConfig, enabled, top_k, model)

inline bool operator==(const SampleConfig& a, const SampleConfig& b) {
    return a.enabled == b.enabled && a.top_k == b.top_k && a.model == b.model;
}

}  // namespace cortrix::catalog::config_resolver_test

namespace cortrix::catalog {
namespace {

using config_resolver_test::SampleConfig;

TEST(ConfigResolverTest, EmptyNsInheritsGlobal) {
    ConfigResolver<SampleConfig> r;
    SampleConfig global{true, 50, "g"};
    auto out = r.Resolve(global, "");
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), global);
}

TEST(ConfigResolverTest, EmptyObjectNsInheritsGlobal) {
    ConfigResolver<SampleConfig> r;
    SampleConfig global{true, 50, "g"};
    auto out = r.Resolve(global, "{}");
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), global);
}

TEST(ConfigResolverTest, NsOverridesSomeFields) {
    ConfigResolver<SampleConfig> r;
    SampleConfig global{false, 10, "default"};
    // NS sets enabled+top_k, leaves model → inherits global "default".
    auto out = r.Resolve(global, R"({"enabled": true, "top_k": 99})");
    ASSERT_TRUE(out.ok());
    EXPECT_TRUE(out.value().enabled);
    EXPECT_EQ(out.value().top_k, 99);
    EXPECT_EQ(out.value().model, "default");
}

TEST(ConfigResolverTest, RequestOverridesOnlyWhitelistedFields) {
    ConfigResolver<SampleConfig> r;
    r.SetRequestAllowedFields({"enabled"});  // only 'enabled' may be set per-request
    SampleConfig global{false, 10, "default"};
    SampleConfig request{true, 999, "hacker"};  // tries to set all three
    auto out = r.Resolve(global, "{}", &request);
    ASSERT_TRUE(out.ok());
    EXPECT_TRUE(out.value().enabled);          // whitelisted → applied
    EXPECT_EQ(out.value().top_k, 10);          // NOT whitelisted → global kept
    EXPECT_EQ(out.value().model, "default");   // NOT whitelisted → global kept
}

TEST(ConfigResolverTest, RequestIgnoredWhenNoWhitelist) {
    ConfigResolver<SampleConfig> r;  // empty whitelist
    SampleConfig global{false, 10, "default"};
    SampleConfig request{true, 999, "x"};
    auto out = r.Resolve(global, "{}", &request);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), global);  // request fully ignored
}

TEST(ConfigResolverTest, LayerPrecedenceGlobalNsRequest) {
    ConfigResolver<SampleConfig> r;
    r.SetRequestAllowedFields({"top_k"});
    SampleConfig global{false, 1, "g"};
    // NS sets top_k=2 and enabled=true; request (whitelisted top_k) sets top_k=3.
    SampleConfig request{false, 3, "g"};
    auto out = r.Resolve(global, R"({"enabled": true, "top_k": 2})", &request);
    ASSERT_TRUE(out.ok());
    EXPECT_TRUE(out.value().enabled);      // from NS
    EXPECT_EQ(out.value().top_k, 3);       // request beats NS beats global
}

TEST(ConfigResolverTest, MalformedNsJsonIsInvalidConfig) {
    ConfigResolver<SampleConfig> r;
    SampleConfig global;
    auto out = r.Resolve(global, R"({"enabled": )");  // truncated
    EXPECT_FALSE(out.ok());
    EXPECT_TRUE(out.status().message().rfind("CX_ERR_INVALID_CONFIG_JSON", 0) == 0)
        << out.status().message();
}

TEST(ConfigResolverTest, NonObjectNsJsonIsInvalidConfig) {
    ConfigResolver<SampleConfig> r;
    SampleConfig global;
    auto out = r.Resolve(global, R"([1,2,3])");
    EXPECT_FALSE(out.ok());
    EXPECT_TRUE(out.status().message().rfind("CX_ERR_INVALID_CONFIG_JSON", 0) == 0)
        << out.status().message();
}

TEST(ConfigResolverTest, WrongTypedNsFieldIsInvalidConfig) {
    ConfigResolver<SampleConfig> r;
    SampleConfig global;
    // top_k must be int; a string makes from_json<SampleConfig> throw → mapped.
    auto out = r.Resolve(global, R"({"top_k": "not-an-int"})");
    EXPECT_FALSE(out.ok());
    EXPECT_TRUE(out.status().message().rfind("CX_ERR_INVALID_CONFIG_JSON", 0) == 0)
        << out.status().message();
}

}  // namespace
}  // namespace cortrix::catalog
