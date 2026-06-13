#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>

#include "cortrix/security/env_secret_provider.h"
#include "cortrix/security/i_secret_provider.h"

namespace cortrix::security {
namespace {

class EnvSecretProviderTest : public ::testing::Test {
protected:
    void TearDown() override {
        unsetenv("CORTRIX_TEST_SECRET");
        unsetenv("CORTRIX_SECRET_PROVIDER");
    }
};

TEST_F(EnvSecretProviderTest, GetReturnsSetValue) {
    setenv("CORTRIX_TEST_SECRET", "s3cr3t", 1);
    EnvSecretProvider provider;
    auto value = provider.Get("CORTRIX_TEST_SECRET");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "s3cr3t");
}

TEST_F(EnvSecretProviderTest, GetReturnsNulloptWhenUnset) {
    EnvSecretProvider provider;
    auto value = provider.Get("CORTRIX_DEFINITELY_NOT_SET_XYZ");
    EXPECT_FALSE(value.has_value());
}

TEST_F(EnvSecretProviderTest, GetAsyncOkWhenUnsetIsNotError) {
    EnvSecretProvider provider;
    // Unset var -> Ok(nullopt), NOT an error (provider answered; key has no value).
    SecretResult result = provider.GetAsync("CORTRIX_DEFINITELY_NOT_SET_XYZ").get();
    EXPECT_TRUE(result.ok());
    EXPECT_FALSE(result.value().has_value());
}

TEST_F(EnvSecretProviderTest, GetAsyncOkWithValue) {
    setenv("CORTRIX_TEST_SECRET", "abc123", 1);
    EnvSecretProvider provider;
    SecretResult result = provider.GetAsync("CORTRIX_TEST_SECRET").get();
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(*result.value(), "abc123");
}

TEST_F(EnvSecretProviderTest, IsHealthyAlwaysTrue) {
    EnvSecretProvider provider;
    EXPECT_TRUE(provider.IsHealthy());
}

TEST_F(EnvSecretProviderTest, ProviderTypeIsEnv) {
    EnvSecretProvider provider;
    EXPECT_EQ(provider.provider_type(), "env");
}

TEST_F(EnvSecretProviderTest, FactoryDefaultsToEnv) {
    unsetenv("CORTRIX_SECRET_PROVIDER");
    auto provider = CreateSecretProvider();
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->provider_type(), "env");
}

TEST_F(EnvSecretProviderTest, FactoryUnknownFallsBackToEnv) {
    setenv("CORTRIX_SECRET_PROVIDER", "vault-not-yet-supported", 1);
    auto provider = CreateSecretProvider();
    ASSERT_NE(provider, nullptr);
    // Unknown provider is non-fatal in Phase 1 — falls back to env.
    EXPECT_EQ(provider->provider_type(), "env");
}

TEST(SecretErrorTest, ToStringMapsAllValues) {
    EXPECT_STREQ(ToString(SecretError::kNotFound), "not_found");
    EXPECT_STREQ(ToString(SecretError::kProviderUnavailable), "provider_unavailable");
    EXPECT_STREQ(ToString(SecretError::kPermissionDenied), "permission_denied");
    EXPECT_STREQ(ToString(SecretError::kNetworkTimeout), "network_timeout");
    EXPECT_STREQ(ToString(SecretError::kInvalidConfig), "invalid_config");
}

}  // namespace
}  // namespace cortrix::security
