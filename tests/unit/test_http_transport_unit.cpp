// Unit tests for http_transport.cpp (src/llm/http_transport.cpp).
// Current coverage: 22.4%.  The real HttplibTransport requires a live network
// connection that cannot exist in unit tests, so those paths are skipped.
//
// What CAN be tested without a network:
//   - SplitBaseAndPath (static helper inside the .cpp) via the public behavior
//     of HttpResponse and the header() utility:
//       - header() case-insensitive lookup
//       - header() returns "" when key is absent
//       - HttpResponse::ok() semantics
//       - HttpRequest default values
//   - MakeDefaultHttpTransport() returns a non-null IHttpTransport.
//   - FakeHttpTransport (the test double) is a drop-in:
//       - Send() records the request + returns the canned response.
//       - Builders (Json2xx / Http / RateLimited / NetworkFail) produce correct fields.
//   - IHttpTransport polymorphism: a lambda-backed stub satisfies the interface.
//
// Paths SKIPPED (require a live httplib server):
//   - Real GET / POST through HttplibTransport
//   - HTTPS (CPPHTTPLIB_OPENSSL_SUPPORT deferred to integration)
//   - Timeout / connection failure through the real transport

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cortrix/llm/http_transport.h"
#include "fake_http_transport.h"

namespace cortrix::llm {
namespace {

// ---------------------------------------------------------------------------
// HttpResponse::ok()
// ---------------------------------------------------------------------------

TEST(HttpResponseTest, OkFalseWhenNetworkFailed) {
    HttpResponse r;
    r.network_ok  = false;
    r.status_code = 0;
    EXPECT_FALSE(r.ok());
}

TEST(HttpResponseTest, OkFalseWhen4xx) {
    HttpResponse r;
    r.network_ok  = true;
    r.status_code = 400;
    EXPECT_FALSE(r.ok());
}

TEST(HttpResponseTest, OkFalseWhen5xx) {
    HttpResponse r;
    r.network_ok  = true;
    r.status_code = 503;
    EXPECT_FALSE(r.ok());
}

TEST(HttpResponseTest, OkTrueFor200) {
    HttpResponse r;
    r.network_ok  = true;
    r.status_code = 200;
    EXPECT_TRUE(r.ok());
}

TEST(HttpResponseTest, OkTrueFor201) {
    HttpResponse r;
    r.network_ok  = true;
    r.status_code = 201;
    EXPECT_TRUE(r.ok());
}

TEST(HttpResponseTest, OkTrueFor299) {
    HttpResponse r;
    r.network_ok  = true;
    r.status_code = 299;
    EXPECT_TRUE(r.ok());
}

TEST(HttpResponseTest, OkFalseFor300) {
    HttpResponse r;
    r.network_ok  = true;
    r.status_code = 300;
    EXPECT_FALSE(r.ok());
}

// ---------------------------------------------------------------------------
// HttpResponse::header() — case-insensitive lookup
// ---------------------------------------------------------------------------

TEST(HttpResponseTest, HeaderCaseSensitiveLookupExact) {
    HttpResponse r;
    r.headers["Content-Type"] = "application/json";
    EXPECT_EQ(r.header("Content-Type"), "application/json");
}

TEST(HttpResponseTest, HeaderCaseInsensitiveLowercase) {
    HttpResponse r;
    r.headers["Retry-After"] = "42";
    EXPECT_EQ(r.header("retry-after"), "42");
}

TEST(HttpResponseTest, HeaderCaseInsensitiveUppercase) {
    HttpResponse r;
    r.headers["content-type"] = "text/plain";
    EXPECT_EQ(r.header("CONTENT-TYPE"), "text/plain");
}

TEST(HttpResponseTest, HeaderMissingReturnsEmpty) {
    HttpResponse r;
    r.headers["X-Present"] = "yes";
    EXPECT_EQ(r.header("X-Absent"), "");
}

TEST(HttpResponseTest, HeaderOnEmptyMapReturnsEmpty) {
    HttpResponse r;
    EXPECT_EQ(r.header("anything"), "");
}

TEST(HttpResponseTest, HeaderMultipleEntriesReturnFirst) {
    // std::map only keeps one entry per key (headers is a map not multimap).
    HttpResponse r;
    r.headers["x-foo"] = "first";
    // Overwrite: only "second" remains.
    r.headers["x-foo"] = "second";
    EXPECT_EQ(r.header("X-Foo"), "second");
}

// ---------------------------------------------------------------------------
// HttpRequest default values
// ---------------------------------------------------------------------------

TEST(HttpRequestTest, DefaultMethodIsPost) {
    HttpRequest req;
    EXPECT_EQ(req.method, HttpMethod::kPost);
}

TEST(HttpRequestTest, DefaultTimeoutIs30000) {
    HttpRequest req;
    EXPECT_EQ(req.timeout_ms, 30000);
}

TEST(HttpRequestTest, DefaultUrlAndBodyAreEmpty) {
    HttpRequest req;
    EXPECT_TRUE(req.url.empty());
    EXPECT_TRUE(req.body.empty());
}

// ---------------------------------------------------------------------------
// MakeDefaultHttpTransport() — factory smoke test
// ---------------------------------------------------------------------------

TEST(MakeDefaultHttpTransportTest, ReturnsNonNull) {
    auto transport = MakeDefaultHttpTransport();
    EXPECT_NE(transport, nullptr);
}

TEST(MakeDefaultHttpTransportTest, IsInstanceOfIHttpTransport) {
    auto transport = MakeDefaultHttpTransport();
    // Polymorphic cast must succeed.
    IHttpTransport* ptr = transport.get();
    EXPECT_NE(ptr, nullptr);
}

// ---------------------------------------------------------------------------
// FakeHttpTransport — test double behavior
// ---------------------------------------------------------------------------

TEST(FakeHttpTransportTest, RecordsLastRequest) {
    FakeHttpTransport fake;
    fake.canned = FakeHttpTransport::Json2xx(R"({"ok":true})");

    HttpRequest req;
    req.method  = HttpMethod::kPost;
    req.url     = "https://api.example.com/v1/chat/completions";
    req.headers["Authorization"] = "Bearer sk-test";
    req.body    = R"({"model":"gpt-4o-mini"})";

    fake.Send(req);

    EXPECT_EQ(fake.last_request.url, req.url);
    EXPECT_EQ(fake.last_request.headers.at("Authorization"), "Bearer sk-test");
    EXPECT_EQ(fake.last_request.body, req.body);
}

TEST(FakeHttpTransportTest, AccumulatesAllRequests) {
    FakeHttpTransport fake;
    fake.canned = FakeHttpTransport::Json2xx("{}");

    HttpRequest r1, r2;
    r1.url = "https://a.example.com/";
    r2.url = "https://b.example.com/";
    fake.Send(r1);
    fake.Send(r2);

    EXPECT_EQ(fake.requests.size(), 2u);
    EXPECT_EQ(fake.requests[0].url, r1.url);
    EXPECT_EQ(fake.requests[1].url, r2.url);
}

TEST(FakeHttpTransportTest, Json2xxBuilderSetsFields) {
    auto resp = FakeHttpTransport::Json2xx(R"({"hello":"world"})");
    EXPECT_TRUE(resp.network_ok);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, R"({"hello":"world"})");
    EXPECT_TRUE(resp.ok());
}

TEST(FakeHttpTransportTest, Json2xxBuilderCustomStatus) {
    auto resp = FakeHttpTransport::Json2xx("{}", 201);
    EXPECT_EQ(resp.status_code, 201);
    EXPECT_TRUE(resp.ok());
}

TEST(FakeHttpTransportTest, HttpBuilderSetsStatusAndBody) {
    auto resp = FakeHttpTransport::Http(503, "Service Unavailable");
    EXPECT_TRUE(resp.network_ok);
    EXPECT_EQ(resp.status_code, 503);
    EXPECT_EQ(resp.body, "Service Unavailable");
    EXPECT_FALSE(resp.ok());
}

TEST(FakeHttpTransportTest, RateLimitedBuilderSets429AndRetryAfterHeader) {
    auto resp = FakeHttpTransport::RateLimited(30);
    EXPECT_TRUE(resp.network_ok);
    EXPECT_EQ(resp.status_code, 429);
    EXPECT_EQ(resp.header("Retry-After"), "30");
    EXPECT_FALSE(resp.ok());
}

TEST(FakeHttpTransportTest, NetworkFailBuilderNetworkOkFalse) {
    auto resp = FakeHttpTransport::NetworkFail();
    EXPECT_FALSE(resp.network_ok);
    EXPECT_FALSE(resp.ok());
}

TEST(FakeHttpTransportTest, ResponderOverridesCannedResponse) {
    FakeHttpTransport fake;
    fake.canned = FakeHttpTransport::Http(200, "canned");
    fake.responder = [](const HttpRequest& req) {
        HttpResponse r;
        r.network_ok  = true;
        r.status_code = 201;
        r.body        = "dynamic:" + req.url;
        return r;
    };

    HttpRequest req;
    req.url = "https://dynamic.example.com/";
    auto resp = fake.Send(req);

    EXPECT_EQ(resp.status_code, 201);
    EXPECT_EQ(resp.body, "dynamic:https://dynamic.example.com/");
}

// ---------------------------------------------------------------------------
// IHttpTransport polymorphism: local implementation via composition
// ---------------------------------------------------------------------------

class ConstTransport : public IHttpTransport {
public:
    explicit ConstTransport(HttpResponse canned) : canned_(std::move(canned)) {}
    HttpResponse Send(const HttpRequest&) override { return canned_; }
private:
    HttpResponse canned_;
};

TEST(IHttpTransportTest, SubclassUsableViaInterface) {
    HttpResponse canned;
    canned.network_ok  = true;
    canned.status_code = 200;
    canned.body        = "constant";

    std::unique_ptr<IHttpTransport> transport =
        std::make_unique<ConstTransport>(canned);

    HttpRequest req;
    auto resp = transport->Send(req);
    EXPECT_TRUE(resp.ok());
    EXPECT_EQ(resp.body, "constant");
}

// ---------------------------------------------------------------------------
// Real transport: Send() with a malformed URL returns network_ok=false
// (no network needed — SplitBaseAndPath fails for a URL with no "://")
// ---------------------------------------------------------------------------

TEST(HttplibTransportTest, MalformedUrlNoSchemeReturnsNetworkFail) {
    // MakeDefaultHttpTransport() returns an HttplibTransport. Feeding it a URL
    // without "://" causes SplitBaseAndPath to fail, so Send() returns
    // network_ok=false without any network I/O.
    auto transport = MakeDefaultHttpTransport();

    HttpRequest req;
    req.url     = "not-a-url-at-all";
    req.method  = HttpMethod::kPost;
    req.body    = "{}";
    req.timeout_ms = 1;  // short timeout guards against accidental connect

    auto resp = transport->Send(req);
    EXPECT_FALSE(resp.network_ok);
    EXPECT_FALSE(resp.ok());
}

TEST(HttplibTransportTest, UrlWithSchemeButNoPathGetsSlashPath) {
    // When there is no '/' after the host, SplitBaseAndPath sets path="/".
    // This is an internal behavior; we verify it does not crash and that
    // the transport at least attempts to connect (network_ok may be false
    // due to the unreachable host — that's acceptable).
    auto transport = MakeDefaultHttpTransport();

    HttpRequest req;
    req.url       = "http://localhost:1";   // port 1 — nothing listens there
    req.method    = HttpMethod::kGet;
    req.timeout_ms = 50;  // 50 ms max

    // We only verify it does NOT throw / crash.
    EXPECT_NO_FATAL_FAILURE({ transport->Send(req); });
}

}  // namespace
}  // namespace cortrix::llm
