// Every error response carries error.request_id, error.timestamp and exactly one
// X-Request-Id header equal to the body's id, whichever code path wrote the body:
//   - CompleteErrorEnvelope, the function that fills in what a body is missing;
//   - the route wrappers (WithAuth, NoAuth), which run it after every handler;
//   - the pre-routing stages (rate limit, admin guard), which answer before any
//     route wrapper runs.
#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib.h"

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/middleware/pre_routing.h"
#include "cortrix/server/http_server.h"
#include "cortrix/server/request_context.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

constexpr const char* kId = "0123456789abcdef0123456789abcdef";
constexpr const char* kOtherId = "fedcba9876543210fedcba9876543210";

// The three request fields, checked together.
void ExpectComplete(const httplib::Response& res, const std::string& want_id) {
    ASSERT_EQ(res.get_header_value_count("X-Request-Id"), 1u);
    EXPECT_EQ(res.get_header_value("X-Request-Id"), want_id);
    const json body = json::parse(res.body);
    EXPECT_EQ(body["error"]["request_id"], want_id);
    ASSERT_TRUE(body["error"].contains("timestamp"));
    const std::string ts = body["error"]["timestamp"];
    EXPECT_EQ(ts.size(), 24u) << ts;  // 2026-11-08T01:02:03.456Z
    EXPECT_EQ(ts.back(), 'Z') << ts;
}

httplib::Response ErrorResponse(int status, const std::string& body,
                                const std::string& content_type = "application/json") {
    httplib::Response res;
    res.status = status;
    res.set_content(body, content_type);
    return res;
}

// --- CompleteErrorEnvelope ---------------------------------------------------------

TEST(CompleteErrorEnvelope, FillsAllThreeWhenNoneIsThere) {
    auto res = ErrorResponse(502, R"({"error":{"code":"CX_ERR_EXAMPLE","message":"m"}})");
    CompleteErrorEnvelope(res, kId);
    ExpectComplete(res, kId);
    const json e = json::parse(res.body)["error"];
    EXPECT_EQ(e["code"], "CX_ERR_EXAMPLE") << "the handler's fields are kept";
    EXPECT_EQ(e["message"], "m");
}

TEST(CompleteErrorEnvelope, UsesTheIdAlreadyInTheHeader) {
    auto res = ErrorResponse(400, R"({"error":{"code":"CX_ERR_EXAMPLE"}})");
    res.set_header("X-Request-Id", kOtherId);
    CompleteErrorEnvelope(res, kId);
    ExpectComplete(res, kOtherId);
}

TEST(CompleteErrorEnvelope, UsesTheIdAlreadyInTheBody) {
    auto res = ErrorResponse(
        400, std::string(R"({"error":{"code":"CX_ERR_EXAMPLE","request_id":")") + kOtherId +
                 R"("}})");
    CompleteErrorEnvelope(res, kId);
    ExpectComplete(res, kOtherId);
}

TEST(CompleteErrorEnvelope, LeavesACompleteResponseByteForByte) {
    httplib::Response res;
    WriteJsonError(res, Status::InvalidArgument("bad"), kId);
    const std::string before = res.body;
    CompleteErrorEnvelope(res, kOtherId);
    EXPECT_EQ(res.body, before);
    ExpectComplete(res, kId);
}

TEST(CompleteErrorEnvelope, LeavesEverythingElseAlone) {
    const std::vector<std::pair<int, std::string>> cases = {
        {200, R"({"error":{"code":"CX_ERR_EXAMPLE"}})"},  // a success with an error field
        {207, R"({"error":{"code":"CX_ERR_EXAMPLE"}})"},
        {400, R"({"error":"a plain string"})"},
        {400, R"({"error":null})"},
        {400, R"({"message":"no error object"})"},
        {400, R"(not json)"},
        {400, R"([1,2,3])"},
    };
    for (const auto& [status, body] : cases) {
        auto res = ErrorResponse(status, body);
        CompleteErrorEnvelope(res, kId);
        EXPECT_EQ(res.body, body) << status << " " << body;
        EXPECT_FALSE(res.has_header("X-Request-Id")) << status << " " << body;
    }
    auto text = ErrorResponse(500, R"({"error":{"code":"X"}})", "text/plain");
    CompleteErrorEnvelope(text, kId);
    EXPECT_EQ(text.body, R"({"error":{"code":"X"}})") << "only JSON bodies are completed";

    httplib::Response empty;
    empty.status = 405;
    CompleteErrorEnvelope(empty, kId);
    EXPECT_TRUE(empty.body.empty());
    EXPECT_FALSE(empty.has_header("X-Request-Id"));
}

// WriteJsonResponse after the header is already set must not add a second line.
TEST(CompleteErrorEnvelope, WriteJsonResponseKeepsAnExistingHeaderSingle) {
    httplib::Response res;
    res.set_header("X-Request-Id", kId);
    WriteJsonResponse(res, 200, json{{"ok", true}}, kId);
    EXPECT_EQ(res.get_header_value_count("X-Request-Id"), 1u);
}

// --- the route wrappers -------------------------------------------------------------

// A handler that writes its own error body without any request field, the way
// several feature routes build theirs.
HttpHandler BareErrorHandler() {
    return [](const httplib::Request&, httplib::Response& res, const RequestContext&) {
        res.status = 409;
        res.set_content(R"({"error":{"code":"CX_ERR_EXAMPLE","message":"conflict"}})",
                        "application/json");
    };
}

TEST(ErrorEnvelopeWrappers, NoAuthCompletesAHandlersBareErrorBody) {
    auto h = NoAuth(BareErrorHandler());
    httplib::Request req;
    httplib::Response res;
    h(req, res);
    EXPECT_EQ(res.status, 409);
    ExpectComplete(res, json::parse(res.body)["error"]["request_id"]);
    EXPECT_EQ(res.get_header_value("X-Request-Id").size(), 32u);
}

TEST(ErrorEnvelopeWrappers, WithAuthCompletesAHandlersBareErrorBody) {
    ApiKeyAuth auth;
    ApiKeyConfig kc;
    kc.key_hash = ApiKeyAuth::HashKey("test-key");
    kc.tenant_id = "default_tenant";
    kc.permissions = kPermRead;
    kc.expires_at = 0;
    auth.LoadKeys({kc});
    auto h = WithAuth(auth, kPermRead, BareErrorHandler());
    httplib::Request req;
    req.set_header("Authorization", "Bearer test-key");
    httplib::Response res;
    h(req, res);
    EXPECT_EQ(res.status, 409);
    ExpectComplete(res, json::parse(res.body)["error"]["request_id"]);
    EXPECT_EQ(res.get_header_value("X-Request-Id").size(), 32u);
}

TEST(ErrorEnvelopeWrappers, ASuccessIsNotTouched) {
    auto h = NoAuth([](const httplib::Request&, httplib::Response& res, const RequestContext&) {
        res.status = 200;
        res.set_content(R"({"ok":true})", "application/json");
    });
    httplib::Request req;
    httplib::Response res;
    h(req, res);
    EXPECT_EQ(res.body, R"({"ok":true})");
    EXPECT_FALSE(res.has_header("X-Request-Id"));
}

// --- the pre-routing stages ---------------------------------------------------------

TEST(ErrorEnvelopePreRouting, AStageThatAnswersGetsTheRequestFields) {
    httplib::Server svr;
    svr.Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"ok":true})", "application/json");
    });
    // A stage that answers with a bare error body, as the rate limiter and the admin
    // guard build theirs.
    middleware::InstallPreRouting(
        svr, {[](const httplib::Request&, httplib::Response& res) {
            res.status = 429;
            res.set_content(R"({"error":{"code":"CX_ERR_RATE_LIMITED"}})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }});
    const int port = svr.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    std::thread t([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();

    httplib::Client cli("127.0.0.1", port);
    auto res = cli.Get("/api/v1/health");
    svr.stop();
    t.join();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 429);
    ASSERT_EQ(res->get_header_value_count("X-Request-Id"), 1u);
    const json e = json::parse(res->body)["error"];
    EXPECT_EQ(e["code"], "CX_ERR_RATE_LIMITED");
    EXPECT_EQ(e["request_id"], res->get_header_value("X-Request-Id"));
    EXPECT_TRUE(e.contains("timestamp"));
}

}  // namespace
}  // namespace cortrix
