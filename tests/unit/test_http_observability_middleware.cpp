#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/http_observability_middleware.h"

// S2 coverage: the HTTP observability middleware (§6.1) — three-header parse +
// validation, invalid-header warning + cortrix_invalid_header_total metric,
// server-generated trace_id fallback, and the §6.2 CORS allowlists. Plus the
// UUID generator's validity (round-trips through ObservabilityValidator).
namespace cortrix::agent_trace {
namespace {

using observability::HttpHeaders;
using observability::ObservabilityValidator;

class HttpObsMiddlewareTest : public ::testing::Test {
protected:
    void SetUp() override { AgentTraceMetrics::Instance().ResetForTest(); }
    void TearDown() override { AgentTraceMetrics::Instance().ResetForTest(); }
};

TEST_F(HttpObsMiddlewareTest, AllValidHeadersPopulateContext) {
    HttpHeaders h;
    h.values["X-Session-Id"] = "sess-abc";
    h.values["X-Trace-Id"] = "trace-xyz";
    h.values["X-Agent-Id"] = "bot-1";
    // Deterministic generator so we can assert it was NOT used.
    HttpObservabilityMiddleware mw([] { return std::string("GENERATED"); });
    auto r = mw.Process(h);

    EXPECT_TRUE(r.warnings.empty());
    EXPECT_FALSE(r.generated_trace_id);
    ASSERT_TRUE(r.context.session_id.has_value());
    EXPECT_EQ(*r.context.session_id, "sess-abc");
    ASSERT_TRUE(r.context.agent_id.has_value());
    EXPECT_EQ(*r.context.agent_id, "bot-1");
    ASSERT_NE(r.context.GetTraceContext(), nullptr);
    EXPECT_EQ(r.context.GetTraceContext()->trace_id, "trace-xyz");
    EXPECT_GT(r.context.created_at, 0);
}

TEST_F(HttpObsMiddlewareTest, MissingTraceIdGeneratesFallback) {
    HttpHeaders h;
    h.values["X-Session-Id"] = "sess-1";
    // no X-Trace-Id.
    HttpObservabilityMiddleware mw([] { return std::string("gen-trace-1"); });
    auto r = mw.Process(h);
    EXPECT_TRUE(r.generated_trace_id);
    ASSERT_NE(r.context.GetTraceContext(), nullptr);
    EXPECT_EQ(r.context.GetTraceContext()->trace_id, "gen-trace-1");
    EXPECT_TRUE(r.warnings.empty());  // absent is not a warning, only invalid is
}

TEST_F(HttpObsMiddlewareTest, InvalidHeadersWarnAndMetricAndDrop) {
    HttpHeaders h;
    h.values["X-Session-Id"] = "has space";    // invalid
    h.values["X-Trace-Id"] = "bad;semicolon";  // invalid → also triggers fallback
    h.values["X-Agent-Id"] = "ok-agent";       // valid
    HttpObservabilityMiddleware mw([] { return std::string("fallback-trace"); });
    auto r = mw.Process(h);

    // session dropped (invalid) → warning + metric; agent kept.
    EXPECT_FALSE(r.context.session_id.has_value());
    ASSERT_TRUE(r.context.agent_id.has_value());
    EXPECT_EQ(*r.context.agent_id, "ok-agent");
    // invalid trace → fallback generated.
    EXPECT_TRUE(r.generated_trace_id);
    EXPECT_EQ(r.context.GetTraceContext()->trace_id, "fallback-trace");

    // warnings list both invalid headers.
    EXPECT_EQ(r.warnings.size(), 2u);
    // metric counted both.
    auto& m = AgentTraceMetrics::Instance();
    EXPECT_EQ(m.InvalidHeaderCount(AgentTraceMetrics::HeaderName::kSessionId), 1u);
    EXPECT_EQ(m.InvalidHeaderCount(AgentTraceMetrics::HeaderName::kTraceId), 1u);
    EXPECT_EQ(m.InvalidHeaderCount(AgentTraceMetrics::HeaderName::kAgentId), 0u);
}

TEST_F(HttpObsMiddlewareTest, EmptyHeadersStillGenerateTrace) {
    HttpObservabilityMiddleware mw([] { return std::string("only-trace"); });
    auto r = mw.Process(HttpHeaders{});
    EXPECT_FALSE(r.context.session_id.has_value());
    EXPECT_FALSE(r.context.agent_id.has_value());
    EXPECT_TRUE(r.generated_trace_id);
    EXPECT_TRUE(r.warnings.empty());
}

// The default UUID generator yields a value that passes ObservabilityValidator
// (so the generated trace_id is itself a valid identity) + is unique per call.
TEST_F(HttpObsMiddlewareTest, DefaultUuidIsValidAndUnique) {
    std::string a = GenerateUuidV4();
    std::string b = GenerateUuidV4();
    EXPECT_NE(a, b);
    EXPECT_EQ(a.size(), 36u);  // 8-4-4-4-12 + 4 dashes
    EXPECT_TRUE(ObservabilityValidator::ValidateTraceId(a).ok());
    // version nibble == '4', variant nibble in {8,9,a,b}.
    EXPECT_EQ(a[14], '4');
    EXPECT_TRUE(a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b');
}

TEST_F(HttpObsMiddlewareTest, DefaultCtorUsesRealUuid) {
    HttpObservabilityMiddleware mw;  // default generator
    auto r = mw.Process(HttpHeaders{});
    ASSERT_NE(r.context.GetTraceContext(), nullptr);
    EXPECT_TRUE(ObservabilityValidator::ValidateTraceId(
                    r.context.GetTraceContext()->trace_id).ok());
}

TEST_F(HttpObsMiddlewareTest, CorsAllowlists) {
    const auto& headers = HttpObservabilityMiddleware::CorsAllowedHeaders();
    for (const char* h : {"X-Session-Id", "X-Trace-Id", "X-Agent-Id",
                          "Content-Type", "Authorization"}) {
        EXPECT_NE(std::find(headers.begin(), headers.end(), h), headers.end())
            << "CORS allowed_headers missing " << h;
    }
    const auto& methods = HttpObservabilityMiddleware::CorsAllowedMethods();
    for (const char* m : {"GET", "POST", "OPTIONS"}) {
        EXPECT_NE(std::find(methods.begin(), methods.end(), m), methods.end())
            << "CORS allowed_methods missing " << m;
    }
}

}  // namespace
}  // namespace cortrix::agent_trace
