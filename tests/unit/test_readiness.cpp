#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <optional>

#include <nlohmann/json.hpp>

#include "cortrix/health/readiness.h"
#include "cortrix/security/i_secret_provider.h"
#include "cortrix/security/secret_provider_readiness.h"

namespace cortrix::health {
namespace {

using nlohmann::json;

// ---- ReadinessRegistry aggregation ----

TEST(ReadinessRegistryTest, AllReadyIsReady) {
    ReadinessRegistry reg;
    reg.Register("catalog", [] {
        ComponentReadiness r;
        r.ready = true;
        r.detail["bloom_filter_ready"] = true;
        return r;
    });
    reg.Register("vector_index", [] {
        return ComponentReadiness{true, json{{"model_loaded", true}}};
    });

    bool overall = false;
    json report = reg.BuildReport(&overall);

    EXPECT_TRUE(overall);
    EXPECT_EQ(report["status"], "ready");
    EXPECT_EQ(report["components"]["catalog"]["status"], "ok");
    EXPECT_EQ(report["components"]["catalog"]["bloom_filter_ready"], true);
    EXPECT_EQ(report["components"]["vector_index"]["status"], "ok");
    EXPECT_FALSE(report.contains("warnings"));  // no warnings when all ok
}

TEST(ReadinessRegistryTest, OneNotReadyMakesOverallNotReady) {
    ReadinessRegistry reg;
    reg.Register("catalog", [] { return ComponentReadiness{true, {}}; });
    reg.Register("vector_index", [] {
        ComponentReadiness r;
        r.ready = false;
        r.detail["reason"] = "model_loading";
        r.detail["progress"] = 0.65;
        return r;
    });

    bool overall = true;
    json report = reg.BuildReport(&overall);

    EXPECT_FALSE(overall);
    EXPECT_EQ(report["status"], "not_ready");
    EXPECT_EQ(report["components"]["vector_index"]["status"], "not_ready");
    EXPECT_EQ(report["components"]["vector_index"]["reason"], "model_loading");
    ASSERT_TRUE(report.contains("warnings"));
    EXPECT_EQ(report["warnings"].size(), 1u);
}

TEST(ReadinessRegistryTest, EmptyRegistryIsReady) {
    ReadinessRegistry reg;
    bool overall = false;
    json report = reg.BuildReport(&overall);
    // No components -> vacuously ready (server up, nothing to gate on).
    EXPECT_TRUE(overall);
    EXPECT_EQ(report["status"], "ready");
    EXPECT_EQ(report["components"].size(), 0u);
}

TEST(ReadinessRegistryTest, NullComponentIgnored) {
    ReadinessRegistry reg;
    reg.Register(std::shared_ptr<IReadyComponent>(nullptr));
    EXPECT_EQ(reg.size(), 0u);
}

// ---- SecretProviderReadiness adapter (F20-6) ----

namespace {
class FakeProvider : public security::ISecretProvider {
public:
    explicit FakeProvider(bool healthy) : healthy_(healthy) {}
    std::future<security::SecretResult> GetAsync(const std::string&) override {
        std::promise<security::SecretResult> p;
        p.set_value(security::SecretResult::Ok(std::nullopt));
        return p.get_future();
    }
    bool IsHealthy() const override { return healthy_; }
    std::string provider_type() const override { return "env"; }
private:
    bool healthy_;
};
}  // namespace

TEST(SecretProviderReadinessTest, ReadyWhenProviderHealthy) {
    auto provider = std::make_shared<FakeProvider>(true);
    security::SecretProviderReadiness component(provider);

    EXPECT_EQ(component.name(), "secret_provider");
    ComponentReadiness r = component.CheckReady();
    EXPECT_TRUE(r.ready);
    EXPECT_EQ(r.detail["provider_type"], "env");
}

TEST(SecretProviderReadinessTest, NotReadyWhenProviderUnhealthy) {
    auto provider = std::make_shared<FakeProvider>(false);
    security::SecretProviderReadiness component(provider);
    ComponentReadiness r = component.CheckReady();
    EXPECT_FALSE(r.ready);
}

TEST(SecretProviderReadinessTest, IntegratesIntoRegistry) {
    ReadinessRegistry reg;
    auto provider = std::make_shared<FakeProvider>(true);
    reg.Register(std::make_shared<security::SecretProviderReadiness>(provider));

    bool overall = false;
    json report = reg.BuildReport(&overall);
    EXPECT_TRUE(overall);
    EXPECT_EQ(report["components"]["secret_provider"]["status"], "ok");
    EXPECT_EQ(report["components"]["secret_provider"]["provider_type"], "env");
}

}  // namespace
}  // namespace cortrix::health
