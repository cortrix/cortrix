#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/llm/http_transport.h"

namespace cortrix::llm {

/// Test double for IHttpTransport (enricher Wave B). Lets OpenAiLlmClient tests drive
/// the full request-build + response-parse path with no network. Either set a
/// canned response, or a responder lambda for per-request control; records the
/// last request so tests can assert the OpenAI request shape.
class FakeHttpTransport : public IHttpTransport {
public:
    HttpResponse Send(const HttpRequest& request) override {
        last_request = request;
        requests.push_back(request);
        if (responder) return responder(request);
        return canned;
    }

    // Canned response (default: a connect failure → network_ok=false).
    HttpResponse canned{};
    // Optional per-request responder (overrides `canned` when set).
    std::function<HttpResponse(const HttpRequest&)> responder;

    HttpRequest last_request{};
    std::vector<HttpRequest> requests;

    // --- builders for common responses ---
    static HttpResponse Json2xx(std::string body, int status = 200) {
        HttpResponse r;
        r.network_ok = true;
        r.status_code = status;
        r.body = std::move(body);
        return r;
    }
    static HttpResponse Http(int status, std::string body = "") {
        HttpResponse r;
        r.network_ok = true;
        r.status_code = status;
        r.body = std::move(body);
        return r;
    }
    static HttpResponse RateLimited(int retry_after_sec) {
        HttpResponse r = Http(429);
        r.headers.emplace("Retry-After", std::to_string(retry_after_sec));
        return r;
    }
    static HttpResponse NetworkFail() {
        return HttpResponse{};  // network_ok=false
    }
};

}  // namespace cortrix::llm
