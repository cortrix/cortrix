#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "httplib.h"

#include <chrono>
#include <memory>
#include <thread>

#include "cortrix/common/status.h"
#include "cortrix/server/http_server.h"
#include "cortrix/server/routes/agent_proxy_routes.h"

// Error-path coverage for the retrieval-chain and pgcortrix error
// identities that the GEN-Agent error envelope surfaces via the sec.3 SoT map
// (http_server.cpp Sdk3Map / ResolveError). Each of these CX_ERR_* codes had ZERO
// referencing test. The codes are NOT thrown by a service object; they are the
// curated L2 high-frequency identities whose true {category, retryable} the coarse
// StatusCode cannot reconstruct, so the only place they are exercised is the
// envelope-resolution path. The genuine precondition is therefore: a cortrix::Status
// whose message carries the "<CX_ERR_*>: detail" token (the CatalogStatus /
// TenantStatus / MakeAuthError convention) reaches WriteJsonError, which lifts the
// token, looks it up in the sec.3 map, and emits the resolved category/retryable.
// We assert all three observable surfaces: envelope "code", "category", "retryable".
namespace cortrix {
namespace {

using json = nlohmann::json;

// Build a Status carrying the rich CX_ERR_* token exactly as the boundary does:
// "<CODE>: <human-readable detail>". The StatusCode is the coarse HTTP carrier; the
// precise identity + category/retryable come from the token via the sec.3 map.
nlohmann::json ResolveEnvelope(StatusCode sc, const std::string& token,
                               const std::string& detail) {
    httplib::Response res;
    WriteJsonError(res, Status(sc, token + ": " + detail), "req-errpath");
    return json::parse(res.body);
}

// ---------------------------------------------------------------------------
// Agent retrieval-chain LLM identities (sec.3.5). The map deliberately restores
// quota/timeout/transient category for codes the StatusCode would mislabel.
// ---------------------------------------------------------------------------

TEST(ErrPathF48Test, LlmTimeoutResolvesTimeoutRetryable) {
    auto body = ResolveEnvelope(StatusCode::kInternal, "CX_ERR_F48_LLM_TIMEOUT",
                                "upstream LLM deadline exceeded");
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F48_LLM_TIMEOUT");
    EXPECT_EQ(body["error"]["category"], "timeout");
    EXPECT_EQ(body["error"]["retryable"], true);
}

TEST(ErrPathF48Test, LlmQuotaExceededResolvesQuotaRetryable) {
    auto body = ResolveEnvelope(StatusCode::kInternal, "CX_ERR_F48_LLM_QUOTA_EXCEEDED",
                                "provider token quota exhausted");
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F48_LLM_QUOTA_EXCEEDED");
    EXPECT_EQ(body["error"]["category"], "quota");
    EXPECT_EQ(body["error"]["retryable"], true);
}

TEST(ErrPathF48Test, LlmUnavailableResolvesTransientRetryable) {
    auto body = ResolveEnvelope(StatusCode::kUnavailable, "CX_ERR_F48_LLM_UNAVAILABLE",
                                "LLM endpoint down");
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F48_LLM_UNAVAILABLE");
    EXPECT_EQ(body["error"]["category"], "transient");
    EXPECT_EQ(body["error"]["retryable"], true);
}

TEST(ErrPathF48Test, RagFailedResolvesTransientRetryable) {
    auto body = ResolveEnvelope(StatusCode::kInternal, "CX_ERR_F48_RAG_FAILED",
                                "retrieval-augmented generation step failed");
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F48_RAG_FAILED");
    EXPECT_EQ(body["error"]["category"], "transient");
    EXPECT_EQ(body["error"]["retryable"], true);
}

// Regression guard: without the sec.3 map these would coarsen onto the StatusCode
// default (kInternal -> transient). Use kInvalidArgument (would otherwise resolve to
// "permanent"/false) to prove the map's quota/timeout override actually fires and is
// not an accidental match with the StatusCode fallback.
TEST(ErrPathF48Test, MapOverridesStatusCodeFallback) {
    auto quota = ResolveEnvelope(StatusCode::kInvalidArgument,
                                 "CX_ERR_F48_LLM_QUOTA_EXCEEDED", "x");
    // StatusCode fallback for kInvalidArgument is permanent/false; the map forces quota/true.
    EXPECT_EQ(quota["error"]["category"], "quota");
    EXPECT_EQ(quota["error"]["retryable"], true);
}

// ---------------------------------------------------------------------------
// Pgcortrix invalid-filter identity (sec.3.4): permanent / non-retryable.
// ---------------------------------------------------------------------------

TEST(ErrPathF14Test, InvalidFilterResolvesPermanentNonRetryable) {
    auto body = ResolveEnvelope(StatusCode::kInvalidArgument, "CX_ERR_F14_INVALID_FILTER",
                                "metadata filter expression failed to parse");
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F14_INVALID_FILTER");
    EXPECT_EQ(body["error"]["category"], "permanent");
    EXPECT_EQ(body["error"]["retryable"], false);
}

// Even with a transient-looking StatusCode the map pins pgcortrix to permanent/false
// (a malformed filter is a client fault — blind retry cannot help).
TEST(ErrPathF14Test, InvalidFilterPinnedPermanentEvenOnInternalStatus) {
    auto body = ResolveEnvelope(StatusCode::kInternal, "CX_ERR_F14_INVALID_FILTER", "x");
    EXPECT_EQ(body["error"]["category"], "permanent");
    EXPECT_EQ(body["error"]["retryable"], false);
}

// ---------------------------------------------------------------------------
// CX_ERR_AGENT_UPSTREAM — the agent reverse proxy returns 502 + this code when the
// upstream agent service is unreachable (agent_proxy_routes.cpp:160-166, buffered
// path). The handler constructs its own httplib::Client to agent_base_url, so the
// genuine precondition is simply pointing the proxy at a dead port: any buffered
// /api/v1/agent/* request then fails to connect and surfaces the code. We use the
// buffered surface (/api/v1/agent/health) — deterministic, unlike the SSE /chat path.
// ---------------------------------------------------------------------------
TEST(ErrPathAgentUpstreamTest, UnreachableUpstreamReturnsAgentUpstream) {
    // A port nothing listens on -> the proxy's upstream connect fails.
    const std::string dead_upstream = "http://127.0.0.1:9";  // port 9 (discard) — refused
    httplib::Server svr;
    RegisterAgentProxyRoutes(svr, dead_upstream);

    const int port = 19800 + (getpid() % 200);
    std::thread th([&] { svr.listen("127.0.0.1", port); });
    for (int i = 0; i < 50 && !svr.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    httplib::Client cli("127.0.0.1", port);
    auto res = cli.Get("/api/v1/agent/health");

    svr.stop();
    if (th.joinable()) th.join();

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 502) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_AGENT_UPSTREAM") << res->body;
}

}  // namespace
}  // namespace cortrix
