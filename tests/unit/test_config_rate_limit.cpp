#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "cortrix/config/config.h"

namespace cortrix {
namespace {

bool ContainsError(const std::vector<std::string>& errors, const std::string& needle) {
    for (const auto& e : errors) {
        if (e.find(needle) != std::string::npos) return true;
    }
    return false;
}

class ConfigRateLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        yaml_path_ = "/tmp/cortrix_test_rate_limit_" + std::to_string(getpid()) + ".yaml";
        unsetenv("CORTRIX_RATE_LIMIT_ENABLED");
    }

    void TearDown() override {
        std::remove(yaml_path_.c_str());
        unsetenv("CORTRIX_RATE_LIMIT_ENABLED");
    }

    CortrixConfig LoadYaml(const std::string& content) {
        std::ofstream f(yaml_path_);
        f << content;
        f.close();
        return LoadConfig(yaml_path_);
    }

    std::string yaml_path_;
};

TEST_F(ConfigRateLimitTest, OffByDefaultWithTheDesignBuckets) {
    const CortrixConfig config = LoadConfig("");
    EXPECT_FALSE(config.rate_limit.enabled);
    EXPECT_DOUBLE_EQ(config.rate_limit.per_ip.rate_per_sec, 1.0);
    EXPECT_DOUBLE_EQ(config.rate_limit.per_ip.burst, 100.0);
    EXPECT_DOUBLE_EQ(config.rate_limit.per_api_key.rate_per_sec, 5.0);
    EXPECT_DOUBLE_EQ(config.rate_limit.per_api_key.burst, 500.0);
    EXPECT_DOUBLE_EQ(config.rate_limit.global.rate_per_sec, 1000.0);
    EXPECT_DOUBLE_EQ(config.rate_limit.global.burst, 1500.0);
    EXPECT_FALSE(ContainsError(ValidateConfig(config), "rate_limit"));
}

TEST_F(ConfigRateLimitTest, YamlTurnsItOnAndSizesTheBuckets) {
    const CortrixConfig config = LoadYaml(R"(
rate_limit:
  enabled: true
  per_ip: { rate_per_sec: 20, burst: 400 }
  global: { burst: 3000 }
)");
    EXPECT_TRUE(config.rate_limit.enabled);
    EXPECT_DOUBLE_EQ(config.rate_limit.per_ip.rate_per_sec, 20.0);
    EXPECT_DOUBLE_EQ(config.rate_limit.per_ip.burst, 400.0);
    EXPECT_DOUBLE_EQ(config.rate_limit.per_api_key.rate_per_sec, 5.0);  // untouched
    EXPECT_DOUBLE_EQ(config.rate_limit.global.rate_per_sec, 1000.0);     // untouched
    EXPECT_DOUBLE_EQ(config.rate_limit.global.burst, 3000.0);
    EXPECT_FALSE(ContainsError(ValidateConfig(config), "rate_limit"));
}

TEST_F(ConfigRateLimitTest, EnvironmentOverridesTheFile) {
    setenv("CORTRIX_RATE_LIMIT_ENABLED", "true", 1);
    EXPECT_TRUE(LoadYaml("rate_limit:\n  enabled: false\n").rate_limit.enabled);
    setenv("CORTRIX_RATE_LIMIT_ENABLED", "false", 1);
    EXPECT_FALSE(LoadYaml("rate_limit:\n  enabled: true\n").rate_limit.enabled);
}

TEST_F(ConfigRateLimitTest, EnabledBucketsNeedARateAndRoomForOneRequest) {
    CortrixConfig config = LoadConfig("");
    config.rate_limit.enabled = true;
    config.rate_limit.per_ip.rate_per_sec = 0.0;
    config.rate_limit.global.burst = 0.5;
    const auto errors = ValidateConfig(config);
    EXPECT_TRUE(ContainsError(errors, "rate_limit.per_ip needs rate_per_sec > 0"));
    EXPECT_TRUE(ContainsError(errors, "rate_limit.global needs rate_per_sec > 0 and burst >= 1"));
    EXPECT_FALSE(ContainsError(errors, "rate_limit.per_api_key"));

    config.rate_limit.enabled = false;  // the buckets are not checked while it is off
    EXPECT_FALSE(ContainsError(ValidateConfig(config), "rate_limit"));
}

}  // namespace
}  // namespace cortrix
