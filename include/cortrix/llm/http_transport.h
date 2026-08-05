#pragma once
#include <map>
#include <memory>
#include <string>

namespace cortrix::llm {

/// HTTP method (S2.1 chat = POST; S4.2 startup probe = GET {endpoint}/models).
enum class HttpMethod { kPost, kGet };

/// One HTTP request to an OpenAI-compatible endpoint (S2.1). Minimal +
/// transport-agnostic.
struct HttpRequest {
    HttpMethod method = HttpMethod::kPost;       ///< POST (chat) / GET (probe)
    std::string url;                            ///< full URL ({endpoint}/chat/completions)
    std::string body;                           ///< JSON request body (POST)
    std::map<std::string, std::string> headers; ///< incl. Authorization / Content-Type
    int timeout_ms = 30000;                     ///< per-call deadline
};

/// One HTTP response (S2.1). `network_ok` distinguishes a transport-level failure
/// (DNS / connect / TLS / timeout — status_code is meaningless) from a completed
/// round-trip that returned an HTTP status (status_code valid, may be 4xx/5xx).
struct HttpResponse {
    bool network_ok = false;                    ///< false = connect/TLS/timeout failed
    int status_code = 0;                        ///< HTTP status (valid iff network_ok)
    std::string transport_error;                ///< transport error detail when network_ok=false
    std::string body;                           ///< response body
    std::map<std::string, std::string> headers; ///< response headers (e.g. Retry-After)

    /// Convenience: a successful 2xx round-trip.
    bool ok() const { return network_ok && status_code >= 200 && status_code < 300; }

    /// Case-insensitive header lookup; returns "" when absent. (HTTP header names
    /// are case-insensitive; servers vary on "Retry-After" vs "retry-after".)
    std::string header(const std::string& name) const;
};

/// Injectable HTTP transport seam (B-R1 F03 briefing: "make OpenAiLlmClient a
/// mockable interface/seam ... an injectable transport"). OpenAiLlmClient builds the OpenAI
/// request + parses the response (fully unit-testable); the raw byte round-trip
/// goes through this interface so:
///   - downstream features + F03 tests inject a fake;
///   - runtime wiring uses the default network transport against a live endpoint.
///
/// The project build enables cpp-httplib's OpenSSL support so HTTPS endpoints
/// are available at runtime. Standalone tests never touch the network.
class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    virtual HttpResponse Send(const HttpRequest& request) = 0;
};

/// Default transport (S2.1) backed by cpp-httplib. Returns network_ok=false on
/// any transport-level failure so the client maps it to CX_ERR_ENRICHER_LLM_API.
std::unique_ptr<IHttpTransport> MakeDefaultHttpTransport();

}  // namespace cortrix::llm
