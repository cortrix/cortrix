#include <gtest/gtest.h>

#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "cortrix/observability/observability_context.h"

namespace cortrix::observability {
namespace {

TEST(ObservabilityContextTest, SetGetClearTraceContext) {
    auto& ctx = ObservabilityContext::ThreadLocal();
    ctx.ClearTraceContext();
    EXPECT_EQ(ctx.GetTraceContext(), nullptr);

    ctx.SetTraceContext(TraceContext{"trace-1", "span-1", 1});
    ASSERT_NE(ctx.GetTraceContext(), nullptr);
    EXPECT_EQ(ctx.GetTraceContext()->trace_id, "trace-1");
    EXPECT_EQ(ctx.GetTraceContext()->trace_flags, 1);

    ctx.ClearTraceContext();
    EXPECT_EQ(ctx.GetTraceContext(), nullptr);
}

// Each thread has its own ObservabilityContext: a worker thread starts clean and
// its context never leaks into (or from) the main thread.
TEST(ObservabilityContextTest, ThreadLocalIsolation) {
    ObservabilityContext::ThreadLocal().SetTraceContext(TraceContext{"main-trace", "s0", 0});

    std::string worker_seen;
    bool worker_started_clean = false;
    std::thread t([&] {
        worker_started_clean = (ObservabilityContext::ThreadLocal().GetTraceContext() == nullptr);
        ObservabilityContext::ThreadLocal().SetTraceContext(TraceContext{"worker-trace", "s1", 0});
        worker_seen = ObservabilityContext::ThreadLocal().GetTraceContext()->trace_id;
    });
    t.join();

    EXPECT_TRUE(worker_started_clean);
    EXPECT_EQ(worker_seen, "worker-trace");
    // The main thread's context is untouched by the worker.
    ASSERT_NE(ObservabilityContext::ThreadLocal().GetTraceContext(), nullptr);
    EXPECT_EQ(ObservabilityContext::ThreadLocal().GetTraceContext()->trace_id, "main-trace");
    ObservabilityContext::ThreadLocal().ClearTraceContext();
}

TEST(ObservabilityContextTest, FormatStructuredInjectsTraceId) {
    auto& ctx = ObservabilityContext::ThreadLocal();
    ctx.ClearTraceContext();

    auto without = nlohmann::json::parse(ctx.FormatStructured(LogLevel::kInfo, "starting"));
    EXPECT_EQ(without["level"], "info");
    EXPECT_EQ(without["msg"], "starting");
    EXPECT_TRUE(without["trace_id"].is_null());

    ctx.SetTraceContext(TraceContext{"4bf92f35", "00f067aa", 1});
    auto with = nlohmann::json::parse(ctx.FormatStructured(LogLevel::kError, "boom"));
    EXPECT_EQ(with["level"], "error");
    EXPECT_EQ(with["trace_id"], "4bf92f35");
    EXPECT_EQ(with["span_id"], "00f067aa");
    ctx.ClearTraceContext();
}

TEST(ObservabilityContextTest, ToStringLogLevelCoversAll) {
    EXPECT_STREQ(ToString(LogLevel::kDebug), "debug");
    EXPECT_STREQ(ToString(LogLevel::kInfo), "info");
    EXPECT_STREQ(ToString(LogLevel::kWarn), "warn");
    EXPECT_STREQ(ToString(LogLevel::kError), "error");
}

TEST(ObservabilityContextTest, LogStructuredEmitsToStderr) {
    auto& ctx = ObservabilityContext::ThreadLocal();
    ctx.ClearTraceContext();
    // Smoke: must not throw and writes the FormatStructured() JSON to stderr.
    testing::internal::CaptureStderr();
    ctx.LogStructured(LogLevel::kWarn, "hello");
    const std::string err = testing::internal::GetCapturedStderr();
    auto j = nlohmann::json::parse(err);  // one JSON line + trailing '\n'
    EXPECT_EQ(j["level"], "warn");
    EXPECT_EQ(j["msg"], "hello");
}

// ===== F13 §5.1 identity extension =====

// Defaults: a fresh context has every identity field unset and created_at 0.
TEST(ObservabilityContextTest, IdentityFieldsDefaultUnset) {
    ObservabilityContext ctx;
    EXPECT_FALSE(ctx.session_id.has_value());
    EXPECT_FALSE(ctx.agent_id.has_value());
    EXPECT_FALSE(ctx.user_id.has_value());
    EXPECT_FALSE(ctx.namespace_id.has_value());
    EXPECT_EQ(ctx.created_at, 0);
    EXPECT_EQ(ctx.GetTraceContext(), nullptr);
}

TEST(ObservabilityValidatorTest, IsValidFormatRules) {
    // Valid: whitelist [a-zA-Z0-9_.:/-], non-empty, within length.
    EXPECT_TRUE(ObservabilityValidator::IsValidFormat("abc-123", 128));
    EXPECT_TRUE(ObservabilityValidator::IsValidFormat("a_b.c:d/e-f", 128));
    EXPECT_TRUE(ObservabilityValidator::IsValidFormat("X", 128));
    EXPECT_TRUE(ObservabilityValidator::IsValidFormat(std::string(128, 'a'), 128));  // exactly max

    // Invalid: empty / over length / disallowed chars.
    EXPECT_FALSE(ObservabilityValidator::IsValidFormat("", 128));
    EXPECT_FALSE(ObservabilityValidator::IsValidFormat(std::string(129, 'a'), 128));  // over max
    EXPECT_FALSE(ObservabilityValidator::IsValidFormat("has space", 128));
    EXPECT_FALSE(ObservabilityValidator::IsValidFormat("semi;colon", 128));
    EXPECT_FALSE(ObservabilityValidator::IsValidFormat("quote\"x", 128));
    EXPECT_FALSE(ObservabilityValidator::IsValidFormat("emoji\xF0\x9F\x98\x80", 128));
}

TEST(ObservabilityValidatorTest, ValidateReturnsValueOrInvalidFilterToken) {
    auto ok = ObservabilityValidator::ValidateSessionId("sess-001");
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.value(), "sess-001");

    // All three validators reject and carry the CX_ERR_F13_INVALID_FILTER token
    // + the field name in the Status message.
    auto bad_s = ObservabilityValidator::ValidateSessionId("bad value");
    ASSERT_FALSE(bad_s.ok());
    EXPECT_EQ(bad_s.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(bad_s.status().message().find("CX_ERR_F13_INVALID_FILTER"), std::string::npos);
    EXPECT_NE(bad_s.status().message().find("X-Session-Id"), std::string::npos);

    auto bad_t = ObservabilityValidator::ValidateTraceId("");
    ASSERT_FALSE(bad_t.ok());
    EXPECT_NE(bad_t.status().message().find("X-Trace-Id"), std::string::npos);

    auto bad_a = ObservabilityValidator::ValidateAgentId(std::string(200, 'a'));
    ASSERT_FALSE(bad_a.ok());
    EXPECT_NE(bad_a.status().message().find("X-Agent-Id"), std::string::npos);
}

TEST(ObservabilityContextTest, FromHttpHeadersValidAndInvalid) {
    HttpHeaders h;
    h.values["X-Session-Id"] = "sess-abc";
    h.values["X-Trace-Id"] = "4bf92f3577b34da6a3ce929d0e0e4736";
    h.values["X-Agent-Id"] = "sales_bot_v2";
    auto ctx = ObservabilityContext::FromHttpHeaders(h);
    ASSERT_TRUE(ctx.session_id.has_value());
    EXPECT_EQ(*ctx.session_id, "sess-abc");
    ASSERT_TRUE(ctx.agent_id.has_value());
    EXPECT_EQ(*ctx.agent_id, "sales_bot_v2");
    ASSERT_NE(ctx.GetTraceContext(), nullptr);
    EXPECT_EQ(ctx.GetTraceContext()->trace_id, "4bf92f3577b34da6a3ce929d0e0e4736");
    EXPECT_GT(ctx.created_at, 0);

    // Invalid headers are dropped (left unset); the factory does not throw.
    HttpHeaders bad;
    bad.values["X-Session-Id"] = "has space";
    bad.values["X-Trace-Id"] = "bad;trace";
    auto ctx2 = ObservabilityContext::FromHttpHeaders(bad);
    EXPECT_FALSE(ctx2.session_id.has_value());
    EXPECT_EQ(ctx2.GetTraceContext(), nullptr);  // middleware supplies the fallback (S2)
    EXPECT_FALSE(ctx2.agent_id.has_value());

    // Absent headers → all unset.
    auto ctx3 = ObservabilityContext::FromHttpHeaders(HttpHeaders{});
    EXPECT_FALSE(ctx3.session_id.has_value());
    EXPECT_FALSE(ctx3.agent_id.has_value());
    EXPECT_EQ(ctx3.GetTraceContext(), nullptr);
}

TEST(ObservabilityContextTest, FromMcpCapabilityTakesResolvedSession) {
    McpSession s;
    s.session_id = "mcp-sess-1";
    s.agent_id = "agent-x";
    s.namespace_id = "sales";
    auto ctx = ObservabilityContext::FromMcpCapability(s);
    ASSERT_TRUE(ctx.session_id.has_value());
    EXPECT_EQ(*ctx.session_id, "mcp-sess-1");
    EXPECT_EQ(*ctx.agent_id, "agent-x");
    EXPECT_EQ(*ctx.namespace_id, "sales");
    EXPECT_GT(ctx.created_at, 0);

    // Empty session_id → unset (defensive; handler always resolves one).
    McpSession empty;
    auto ctx2 = ObservabilityContext::FromMcpCapability(empty);
    EXPECT_FALSE(ctx2.session_id.has_value());
}

TEST(ObservabilityContextTest, SetThreadLocalLoadsIdentityAndTrace) {
    // Reset the thread-local first.
    auto& tl = ObservabilityContext::ThreadLocal();
    tl.ClearTraceContext();
    tl.session_id.reset();
    tl.user_id.reset();

    ObservabilityContext src;
    src.session_id = "sess-load";
    src.user_id = "alice";
    src.namespace_id = "ns1";
    src.created_at = 123456;
    src.SetTraceContext(TraceContext{"trace-load", "span-load", 0});
    src.SetThreadLocal();

    auto& after = ObservabilityContext::ThreadLocal();
    ASSERT_TRUE(after.session_id.has_value());
    EXPECT_EQ(*after.session_id, "sess-load");
    EXPECT_EQ(*after.user_id, "alice");
    EXPECT_EQ(*after.namespace_id, "ns1");
    EXPECT_EQ(after.created_at, 123456);
    ASSERT_NE(after.GetTraceContext(), nullptr);
    EXPECT_EQ(after.GetTraceContext()->trace_id, "trace-load");

    // Installing a context WITHOUT a trace clears the thread-local trace.
    ObservabilityContext no_trace;
    no_trace.session_id = "sess2";
    no_trace.SetThreadLocal();
    EXPECT_EQ(ObservabilityContext::ThreadLocal().GetTraceContext(), nullptr);
    EXPECT_EQ(*ObservabilityContext::ThreadLocal().session_id, "sess2");

    // cleanup
    ObservabilityContext::ThreadLocal().ClearTraceContext();
}

}  // namespace
}  // namespace cortrix::observability
