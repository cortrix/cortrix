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

// --- StartupValidate --------------------------------------------------

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

// --- CreateEnricher fallback wiring + metrics (scenarios) ---------------

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

// --- ToString coverage for every StartupCheck enumerator -------------------

TEST(StartupValidateTest, ToString_Ok) {
    EXPECT_STREQ(ToString(StartupCheck::kOk), "ok");
}

TEST(StartupValidateTest, ToString_NullType) {
    EXPECT_STREQ(ToString(StartupCheck::kNullType), "null_type");
}

TEST(StartupValidateTest, ToString_ApiKeyMissing) {
    EXPECT_STREQ(ToString(StartupCheck::kApiKeyMissing), "api_key_missing");
}

TEST(StartupValidateTest, ToString_EndpointUnreachable) {
    EXPECT_STREQ(ToString(StartupCheck::kEndpointUnreachable), "endpoint_unreachable");
}

// --- HTTP status-code branches: only 2xx is OK; every other status maps to
// kEndpointUnreachable. Cover the common failure codes individually so the
// `resp.ok()` false branch is exercised with diverse status values. ----------

TEST(StartupValidateTest, ProbeHttp500IsUnreachable) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Http(500, "internal server error");
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kEndpointUnreachable);
    // Probe was issued (exactly one request).
    EXPECT_EQ(t.requests.size(), 1u);
}

TEST(StartupValidateTest, ProbeHttp503IsUnreachable) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Http(503, "service unavailable");
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kEndpointUnreachable);
}

TEST(StartupValidateTest, ProbeHttp404IsUnreachable) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Http(404, "not found");
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kEndpointUnreachable);
}

TEST(StartupValidateTest, ProbeHttp403IsUnreachable) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Http(403, "forbidden");
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kEndpointUnreachable);
}

TEST(StartupValidateTest, ProbeHttp429IsUnreachable) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Http(429, "too many requests");
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kEndpointUnreachable);
}

// A 201 Created is technically 2xx and should be accepted as kOk (range check).
TEST(StartupValidateTest, ProbeHttp201IsOk) {
    auto cfg = LlmCfg();
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Json2xx("{}", 201);
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kOk);
}

// --- Probe URL construction: endpoint without trailing slash -----------------

TEST(StartupValidateTest, ProbeUrlConstructedFromEndpointField) {
    auto cfg = LlmCfg();
    cfg.endpoint = "https://custom.llm.host/api/v2";
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Json2xx("{}");
    StartupValidate(cfg, t);
    ASSERT_EQ(t.requests.size(), 1u);
    EXPECT_EQ(t.last_request.url, "https://custom.llm.host/api/v2/models");
}

// --- Probe timeout carries startup_probe_timeout_ms from config --------------

TEST(StartupValidateTest, ProbeTimeoutFromConfig) {
    auto cfg = LlmCfg();
    cfg.startup_probe_timeout_ms = 7654;
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::NetworkFail();
    StartupValidate(cfg, t);
    ASSERT_EQ(t.requests.size(), 1u);
    EXPECT_EQ(t.last_request.timeout_ms, 7654);
}

// --- Bearer token from api_key -----------------------------------------------

TEST(StartupValidateTest, ProbeAuthHeaderContainsApiKey) {
    auto cfg = LlmCfg();
    cfg.api_key = "sk-secret-key-123";
    llm::FakeHttpTransport t;
    t.canned = llm::FakeHttpTransport::Json2xx("{}");
    StartupValidate(cfg, t);
    ASSERT_EQ(t.requests.size(), 1u);
    EXPECT_EQ(t.last_request.headers.at("Authorization"), "Bearer sk-secret-key-123");
}

// --- kLocalNer type (non-LLM) yields kNullType without probe -----------------

TEST(StartupValidateTest, LocalNerType_IsNullType_NoProbe) {
    EnricherConfig cfg;
    cfg.type = EnricherType::kLocalNer;
    llm::FakeHttpTransport t;
    EXPECT_EQ(StartupValidate(cfg, t), StartupCheck::kNullType);
    EXPECT_TRUE(t.requests.empty());
}

}  // namespace
}  // namespace cortrix::spc
