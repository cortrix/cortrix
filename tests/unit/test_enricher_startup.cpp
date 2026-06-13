#include "cortrix/spc_enricher/enricher_startup.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_error.h"
#include "cortrix/spc_enricher/enricher_metrics.h"
#include "cortrix/spc/parser.h"
#include "fake_http_transport.h"

namespace cortrix::spc {
namespace {

EnricherConfig LlmCfg() {
    EnricherConfig cfg;
    cfg.type = EnricherType::kLlm;
    cfg.enabled = true;
    cfg.api_key = "sk-test";
    cfg.endpoint = "https://api.example.com/v1";
    cfg.model = "gpt-4o-mini";
    return cfg;
}

// --- StartupValidate (§4.1) --------------------------------------------------

TEST(StartupValidateTest, NullTypeIsNullType) {
    EnricherConfig cfg;  // default type = kNull
    llm::FakeHttpTransport t;
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kNullType);
    // No probe issued for the null path.
    EXPECT_TRUE(t.requests.empty());
}

TEST(StartupValidateTest, MissingApiKey) {
    auto cfg = LlmCfg();
    cfg.api_key = "";
    llm::FakeHttpTransport t;
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kApiKeyMissing);
    EXPECT_TRUE(t.requests.empty());  // no probe without a key
}

TEST(StartupValidateTest, ProbeOk) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Json2xx(R"({"data":[{"id":"gpt-4o-mini"}]})");
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kOk);
    // Probe issued a GET to {endpoint}/models with the bearer token.
    ASSERT_EQ(t.requests.size(), 1u);
    EXPECT_EQ(t.last_request.method, llm::HttpMethod::kGet);
    EXPECT_EQ(t.last_request.url, "https://api.example.com/v1/models");
    EXPECT_EQ(t.last_request.headers.at("Authorization"), "Bearer sk-test");
    EXPECT_EQ(t.last_request.timeout_ms, cfg.startup_probe_timeout_ms);
}

TEST(StartupValidateTest, ProbeNetworkFailUnreachable) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::NetworkFail();
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kEndpointUnreachable);
}

TEST(StartupValidateTest, ProbeHttp401Unreachable) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Http(401, "unauthorized");  // non-2xx
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kEndpointUnreachable);
}

// --- CreateEnricher fallback wiring + metrics (§4.4 scenarios) ---------------

class FactoryFallbackTest : public ::testing::Test {
protected:
    void SetUp() override { EnricherMetrics::Instance().ResetForTest(); }
    void TearDown() override { EnricherMetrics::Instance().ResetForTest(); }
};

TEST_F(FactoryFallbackTest, Scenario2ApiKeyMissingFallsBackWithMetric) {
    auto cfg = LlmCfg();
    cfg.api_key = "";
    llm::FakeHttpTransport probe;
    auto e = CreateEnricher(cfg, probe);
    EXPECT_EQ(e->Name(), "NullEnricher");
    EXPECT_EQ(EnricherMetrics::Instance().FallbackToNullCount(
                  EnricherMetrics::FallbackReason::kApiKeyMissing), 1u);
}

TEST_F(FactoryFallbackTest, Scenario3EndpointUnreachableFallsBackWithMetric) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport probe;
    probe.canned = llm::FakeHttpTransport::NetworkFail();
    auto e = CreateEnricher(cfg, probe);
    EXPECT_EQ(e->Name(), "NullEnricher");
    auto& m = EnricherMetrics::Instance();
    EXPECT_EQ(m.FallbackToNullCount(EnricherMetrics::FallbackReason::kEndpointUnreachable), 1u);
    EXPECT_EQ(m.EndpointProbeFailedCount(), 1u);
}

TEST_F(FactoryFallbackTest, ProbeOkBuildsLlmEnricherNoFallbackMetric) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport probe;
    probe.canned = llm::FakeHttpTransport::Json2xx(R"({"data":[]})");
    auto e = CreateEnricher(cfg, probe);
    EXPECT_EQ(e->Name(), "LlmEnricher");
    auto& m = EnricherMetrics::Instance();
    EXPECT_EQ(m.FallbackToNullCount(EnricherMetrics::FallbackReason::kEndpointUnreachable), 0u);
    EXPECT_EQ(m.EndpointProbeFailedCount(), 0u);
}

TEST_F(FactoryFallbackTest, Scenario1NullTypeNoFallbackMetric) {
    EnricherConfig cfg;  // type=null → normal path, no WARN/metric
    llm::FakeHttpTransport probe;
    auto e = CreateEnricher(cfg, probe);
    EXPECT_EQ(e->Name(), "NullEnricher");
    auto& m = EnricherMetrics::Instance();
    EXPECT_EQ(m.FallbackToNullCount(EnricherMetrics::FallbackReason::kApiKeyMissing), 0u);
    EXPECT_EQ(m.FallbackToNullCount(EnricherMetrics::FallbackReason::kEndpointUnreachable), 0u);
}

}  // namespace
}  // namespace cortrix::spc
