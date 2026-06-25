#include "cortrix/llm/http_transport.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include <httplib.h>

#include "cortrix/logging/logging.h"

namespace cortrix::llm {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Split a full URL into (scheme://host[:port], path). cpp-httplib's Client takes
/// a scheme-host-port base separately from the request path.
struct SplitUrl {
    std::string base;  ///< scheme://host[:port]
    std::string path;  ///< /...
    bool ok = false;
};

SplitUrl SplitBaseAndPath(const std::string& url) {
    SplitUrl out;
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return out;
    auto path_pos = url.find('/', scheme_pos + 3);
    if (path_pos == std::string::npos) {
        out.base = url;
        out.path = "/";
    } else {
        out.base = url.substr(0, path_pos);
        out.path = url.substr(path_pos);
    }
    out.ok = true;
    return out;
}

std::pair<int, int> SplitTimeoutMs(int timeout_ms) {
    const int safe_timeout_ms = std::max(timeout_ms, 1);
    return {safe_timeout_ms / 1000, (safe_timeout_ms % 1000) * 1000};
}

/// cpp-httplib transport. The project build enables CPPHTTPLIB_OPENSSL_SUPPORT
/// so OpenAI-compatible HTTPS endpoints can be used by runtime LLM features.
/// Transport failures still return network_ok=false and are mapped by callers to
/// feature-specific LLM API errors.
class HttplibTransport : public IHttpTransport {
public:
    HttpResponse Send(const HttpRequest& request) override {
        HttpResponse resp;
        SplitUrl split = SplitBaseAndPath(request.url);
        if (!split.ok) {
            resp.transport_error = "malformed_url";
            return resp;  // network_ok=false → treated as transport failure
        }

        httplib::Client cli(split.base);
        const auto [timeout_sec, timeout_usec] = SplitTimeoutMs(request.timeout_ms);
        cli.set_connection_timeout(timeout_sec, timeout_usec);
        cli.set_read_timeout(timeout_sec, timeout_usec);
        cli.set_write_timeout(timeout_sec, timeout_usec);

        httplib::Headers headers;
        for (const auto& [k, v] : request.headers) headers.emplace(k, v);

        httplib::Result result =
            request.method == HttpMethod::kGet
                ? cli.Get(split.path, headers)
                : cli.Post(split.path, headers, request.body, "application/json");
        if (!result) {
            // DNS / connect / TLS / timeout — no HTTP status. network_ok stays false.
            resp.transport_error = httplib::to_string(result.error());
            // Surface the low-level transport reason (it was previously swallowed when
            // callers remap to a generic timeout/unavailable — e.g. MEM02 reported
            // "extraction timed out" for an instant TLS/connect failure).
            CORTRIX_LOG_WARN("llm", "transport failure: {} {} -> {}",
                             request.method == HttpMethod::kGet ? "GET" : "POST",
                             split.base, resp.transport_error);
            return resp;
        }
        resp.network_ok = true;
        resp.status_code = result->status;
        resp.body = result->body;
        for (const auto& [k, v] : result->headers) resp.headers.emplace(k, v);
        return resp;
    }
};

}  // namespace

std::string HttpResponse::header(const std::string& name) const {
    const std::string target = ToLower(name);
    for (const auto& [k, v] : headers) {
        if (ToLower(k) == target) return v;
    }
    return "";
}

std::unique_ptr<IHttpTransport> MakeDefaultHttpTransport() {
    return std::make_unique<HttplibTransport>();
}

}  // namespace cortrix::llm
