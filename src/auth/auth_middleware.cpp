#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/logging/logging.h"

namespace cortrix {

httplib::Server::Handler WithAuth(
    ApiKeyAuth& auth,
    int required_permission,
    HttpHandler handler) {

    return [&auth, required_permission, handler](const httplib::Request& req, httplib::Response& res) {
        RequestContext rctx;
        rctx.request_id = GenerateRequestId();
        rctx.start_time = std::chrono::steady_clock::now();

        // If auth is disabled, behave like NoAuth (grant full permissions)
        if (!auth.enabled()) {
            rctx.auth.permissions = kPermRead | kPermWrite | kPermAdmin;
            handler(req, res, rctx);
            return;
        }

        // Extract token from Authorization header or X-API-Key
        std::string auth_header = req.get_header_value("Authorization");
        std::string api_key = req.get_header_value("X-API-Key");

        std::string token;
        if (!auth_header.empty()) {
            const std::string prefix = "Bearer ";
            if (auth_header.size() > prefix.size() &&
                auth_header.substr(0, prefix.size()) == prefix) {
                token = auth_header.substr(prefix.size());
            }
        }
        if (token.empty()) {
            token = api_key;
        }
        if (token.empty()) {
            WriteJsonError(res, Status::Unauthenticated("Missing Authorization header or X-API-Key"), rctx.request_id);
            return;
        }

        // Authenticate
        AuthContext actx;
        Status s = auth.Authenticate(token, &actx);
        if (!s.ok()) {
            WriteJsonError(res, s, rctx.request_id);
            return;
        }

        // Extract namespace from URL path if present (e.g., /api/v1/namespaces/:name)
        std::string ns;
        auto it = req.path_params.find("name");
        if (it != req.path_params.end()) {
            ns = it->second;
        }

        // Authorize
        s = auth.Authorize(actx, ns, required_permission);
        if (!s.ok()) {
            WriteJsonError(res, s, rctx.request_id);
            return;
        }

        rctx.auth = actx;
        handler(req, res, rctx);
    };
}

httplib::Server::Handler NoAuth(HttpHandler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        RequestContext rctx;
        rctx.request_id = GenerateRequestId();
        rctx.start_time = std::chrono::steady_clock::now();
        // Default AuthContext with admin permissions for unauthenticated endpoints
        rctx.auth.permissions = kPermRead | kPermWrite | kPermAdmin;
        handler(req, res, rctx);
    };
}

}  // namespace cortrix
