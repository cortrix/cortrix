#include <cstdint>
#include "cortrix/server/routes/agent_proxy_routes.h"

#include <cctype>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>

#include "cortrix/logging/logging.h"

namespace cortrix {

namespace {

bool IEquals(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

httplib::Headers ForwardableHeaders(const httplib::Request& req) {
    httplib::Headers out;
    for (const auto& [k, v] : req.headers) {
        // Forward auth + cortrix context headers and Accept only; httplib sets
        // Host/Content-Type/Content-Length itself and hop-by-hop headers must
        // not cross a proxy (RFC 7230 section 6.1).
        if (IEquals(k, "Authorization") || IEquals(k, "Accept") ||
            k.rfind("X-Cortrix-", 0) == 0 || k.rfind("x-cortrix-", 0) == 0) {
            out.emplace(k, v);
        }
    }
    return out;
}

std::string UpstreamPath(const httplib::Request& req, const std::string& strip_prefix) {
    std::string path = req.path.substr(strip_prefix.size());
    if (path.empty()) path = "/";
    bool first = true;
    for (const auto& [k, v] : req.params) {
        path += (first ? "?" : "&");
        first = false;
        path += k + "=" + httplib::detail::encode_query_param(v);
    }
    return path;
}

// Streamed pass-through for the SSE chat endpoint: an upstream reader thread
// feeds chunks into a handoff buffer the chunked provider drains, so tokens
// reach the browser as the agent emits them (F48 section 4.3 SSE pass-through).
struct StreamState {
    std::mutex mu;
    std::condition_variable cv;
    std::string pending;
    bool upstream_done = false;
    bool upstream_failed = false;
};

void ProxyChatStreaming(const std::string& base, const httplib::Request& req,
                        httplib::Response& res) {
    auto st = std::make_shared<StreamState>();
    std::string content_type = req.get_header_value("Content-Type");
    if (content_type.empty()) content_type = "application/json";

    std::thread([st, base, body = req.body, headers = ForwardableHeaders(req),
                 content_type] {
        httplib::Client client(base);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(300, 0);  // agent turns can be slow (LLM)
        // This httplib has no Post overload with a ContentReceiver — go through
        // the generic send(Request) which supports streaming receive.
        httplib::Request up;
        up.method = "POST";
        up.path = "/chat";
        up.headers = headers;
        up.set_header("Content-Type", content_type);
        up.body = body;
        up.content_receiver = [st](const char* data, size_t len, uint64_t /*off*/,
                                   uint64_t /*total*/) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->pending.append(data, len);
            st->cv.notify_all();
            return true;
        };
        auto result = client.send(up);
        std::lock_guard<std::mutex> lk(st->mu);
        if (!result || result->status >= 500) st->upstream_failed = true;
        st->upstream_done = true;
        st->cv.notify_all();
    }).detach();

    // Hold the response until first data (or terminal failure) so a dead
    // upstream becomes a clean 502 instead of an empty 200 stream.
    {
        std::unique_lock<std::mutex> lk(st->mu);
        st->cv.wait(lk, [&] { return !st->pending.empty() || st->upstream_done; });
        if (st->upstream_failed && st->pending.empty()) {
            res.status = 502;
            res.set_content(
                R"({"error":{"code":"CX_ERR_AGENT_UPSTREAM","message":"agent service unreachable","retryable":true,"category":"transient"}})",
                "application/json");
            return;
        }
    }

    res.set_chunked_content_provider(
        "text/event-stream",
        [st](size_t /*offset*/, httplib::DataSink& sink) {
            std::string chunk;
            bool finished = false;
            {
                std::unique_lock<std::mutex> lk(st->mu);
                st->cv.wait(lk, [&] { return !st->pending.empty() || st->upstream_done; });
                chunk.swap(st->pending);
                finished = st->upstream_done && st->pending.empty();
            }
            if (!chunk.empty()) {
                if (!sink.write(chunk.data(), chunk.size())) return false;
            }
            if (finished) sink.done();
            return true;
        });
}

// Buffered pass-through for the non-streaming agent surface (config, sessions,
// health, demo-flows).
void ProxyBuffered(const std::string& base, const std::string& strip_prefix,
                   const httplib::Request& req, httplib::Response& res) {
    httplib::Client client(base);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(60, 0);
    const std::string path = UpstreamPath(req, strip_prefix);
    auto headers = ForwardableHeaders(req);
    std::string content_type = req.get_header_value("Content-Type");
    if (content_type.empty()) content_type = "application/json";

    httplib::Result result;
    if (req.method == "GET") {
        result = client.Get(path, headers);
    } else if (req.method == "POST") {
        result = client.Post(path, headers, req.body, content_type);
    } else if (req.method == "PUT") {
        result = client.Put(path, headers, req.body, content_type);
    } else if (req.method == "DELETE") {
        result = client.Delete(path, headers);
    } else {
        res.status = 405;
        return;
    }

    if (!result) {
        res.status = 502;
        res.set_content(
            R"({"error":{"code":"CX_ERR_AGENT_UPSTREAM","message":"agent service unreachable","retryable":true,"category":"transient"}})",
            "application/json");
        return;
    }
    res.status = result->status;
    auto upstream_type = result->get_header_value("Content-Type");
    res.set_content(result->body, upstream_type.empty() ? "application/json" : upstream_type);
}

}  // namespace

void RegisterAgentProxyRoutes(httplib::Server& server, const std::string& agent_base_url) {
    if (agent_base_url.empty()) return;

    // SSE chat — streamed (F48 section 4.3).
    server.Post("/api/v1/agent/chat",
        [agent_base_url](const httplib::Request& req, httplib::Response& res) {
            ProxyChatStreaming(agent_base_url, req, res);
        });

    // Everything else on the two agent prefixes — buffered.
    auto buffered_api = [agent_base_url](const httplib::Request& req, httplib::Response& res) {
        ProxyBuffered(agent_base_url, "/api/v1/agent", req, res);
    };
    auto buffered_cfg = [agent_base_url](const httplib::Request& req, httplib::Response& res) {
        ProxyBuffered(agent_base_url, "/agent", req, res);
    };
    const char* api_pat = R"(/api/v1/agent/.+)";
    const char* cfg_pat = R"(/agent/.+)";
    server.Get(api_pat, buffered_api);
    server.Post(api_pat, buffered_api);
    server.Put(api_pat, buffered_api);
    server.Delete(api_pat, buffered_api);
    server.Get(cfg_pat, buffered_cfg);
    server.Post(cfg_pat, buffered_cfg);
    server.Put(cfg_pat, buffered_cfg);
    server.Delete(cfg_pat, buffered_cfg);

    CORTRIX_LOG_INFO("server", "Agent reverse proxy enabled -> {}", agent_base_url);
}

}  // namespace cortrix
