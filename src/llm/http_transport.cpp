#include "cortrix/llm/http_transport.h"

#include <algorithm>
#include <cctype>

#include <httplib.h>

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

/// cpp-httplib transport. HTTP works as-is; HTTPS requires the build to define
/// CPPHTTPLIB_OPENSSL_SUPPORT (a shared-dependency decision deferred to D3.5 —
/// see http_transport.h). When that macro is absent, an https:// URL cannot be
/// served and Send() reports network_ok=false (the client maps it to
/// CX_ERR_ENRICHER_LLM_API), exactly as a connect failure would.
class HttplibTransport : public IHttpTransport {
public:
    HttpResponse Send(const HttpRequest& request) override {
        HttpResponse resp;
        SplitUrl split = SplitBaseAndPath(request.url);
        if (!split.ok) return resp;  // network_ok=false → treated as transport failure

        httplib::Client cli(split.base);
        cli.set_connection_timeout(0, request.timeout_ms * 1000);  // (sec, usec)
        cli.set_read_timeout(request.timeout_ms / 1000, (request.timeout_ms % 1000) * 1000);
        cli.set_write_timeout(request.timeout_ms / 1000, (request.timeout_ms % 1000) * 1000);

        httplib::Headers headers;
        for (const auto& [k, v] : request.headers) headers.emplace(k, v);

        httplib::Result result =
            request.method == HttpMethod::kGet
                ? cli.Get(split.path, headers)
                : cli.Post(split.path, headers, request.body, "application/json");
        if (!result) {
            // DNS / connect / TLS / timeout — no HTTP status. network_ok stays false.
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
