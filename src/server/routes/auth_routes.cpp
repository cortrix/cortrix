#include "cortrix/server/routes/auth_routes.h"

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/bootstrap_handler.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"

namespace cortrix {

namespace {

using cortrix::auth::ApiKeyInfo;
using cortrix::auth::ApiKeyService;
using cortrix::auth::AuthErrorCode;
using cortrix::auth::BootstrapHandler;
using cortrix::auth::MakeAuthError;

// Write the GEN-Agent 4-field auth error body for `code` (§5.x) with the matching
// HTTP status. Mirrors operations_routes::WriteOplogError — the boundary owns the
// full Agent-friendly envelope, not the bare Status.
void WriteAuthError(httplib::Response& res, AuthErrorCode code,
                    const std::string& message, const std::string& request_id,
                    nlohmann::json structured_data = nlohmann::json::object()) {
    auto err = MakeAuthError(code, std::move(structured_data), message);
    nlohmann::json body;
    body["error"] = agent_friendly::ToJson(err);
    if (!request_id.empty()) {
        body["error"]["request_id"] = request_id;
        res.set_header("X-Request-Id", request_id);
    }
    res.set_content(body.dump(), "application/json");
    res.status = Status(cortrix::auth::AuthErrorToStatusCode(code), "").http_status();
}

// Serialize one ApiKeyInfo to the §2.13.3 list shape (never the plaintext). 0 / ""
// optional fields become JSON null so Agents see an explicit "never" sentinel.
nlohmann::json ApiKeyInfoToJson(const ApiKeyInfo& k) {
    nlohmann::json j;
    j["id"]           = k.id;
    j["name"]         = k.name;
    j["key_prefix"]   = k.key_prefix;
    j["created_at"]   = k.created_at;
    j["last_used_at"] = k.last_used_at == 0 ? nlohmann::json(nullptr)
                                            : nlohmann::json(k.last_used_at);
    j["expires_at"]   = k.expires_at == 0 ? nlohmann::json(nullptr)
                                          : nlohmann::json(k.expires_at);
    j["status"]       = k.status;
    return j;
}

}  // namespace

void RegisterBootstrapRoutes(httplib::Server& server, BootstrapHandler& handler) {
    // GET /api/v1/admin/bootstrap?token=<t> — first-run browser flow (§2.13.2.a).
    // Consumes the shared token, mints the admin key, renders the one-time HTML.
    server.Get("/api/v1/admin/bootstrap",
        [&handler](const httplib::Request& req, httplib::Response& res) {
            const std::string request_id = GenerateRequestId();
            std::string token;
            if (req.has_param("token")) token = req.get_param_value("token");
            if (token.empty()) {
                WriteAuthError(res, AuthErrorCode::kBootstrapTokenInvalid,
                               "missing bootstrap token", request_id,
                               {{"reason", "missing"}});
                return;
            }
            auto r = handler.Consume(token);
            if (!r.ok()) {
                WriteJsonError(res, r.status(), request_id);
                return;
            }
            res.status = 200;
            res.set_content(BootstrapHandler::RenderHtml(r.value().admin_api_key),
                            "text/html");
        });

    // POST /api/v1/admin/bootstrap body {token} — programmatic JSON variant
    // (§2.13.2.b). Same token + same Consume logic; only the rendering differs.
    server.Post("/api/v1/admin/bootstrap",
        [&handler](const httplib::Request& req, httplib::Response& res) {
            const std::string request_id = GenerateRequestId();
            std::string token;
            try {
                auto body = nlohmann::json::parse(req.body);
                if (body.is_object() && body.contains("token") && body["token"].is_string()) {
                    token = body["token"].get<std::string>();
                }
            } catch (...) {
                // fall through to the missing-token error below.
            }
            if (token.empty()) {
                WriteAuthError(res, AuthErrorCode::kBootstrapTokenInvalid,
                               "missing bootstrap token", request_id,
                               {{"reason", "missing"}});
                return;
            }
            auto r = handler.Consume(token);
            if (!r.ok()) {
                WriteJsonError(res, r.status(), request_id);
                return;
            }
            res.status = 200;
            res.set_content(BootstrapHandler::RenderJson(r.value().admin_api_key),
                            "application/json");
        });
}

void RegisterApiKeyRoutes(httplib::Server& server, ApiKeyService& keys,
                          ApiKeyAuth& auth) {
    // POST /api/v1/auth/api-keys — create a key for the caller (§2.13.3). The
    // plaintext is returned ONCE here and never persisted in cleartext.
    server.Post("/api/v1/auth/api-keys",
        WithAuth(auth, kPermAdmin,
            [&keys](const httplib::Request& req, httplib::Response& res,
                    const RequestContext& rctx) {
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteAuthError(res, AuthErrorCode::kInvalidRequest,
                                   "request body must be a JSON object", rctx.request_id);
                    return;
                }
                if (!body.is_object() || !body.contains("name") || !body["name"].is_string()) {
                    WriteAuthError(res, AuthErrorCode::kInvalidRequest,
                                   "string 'name' is required", rctx.request_id);
                    return;
                }
                // expires_at: optional ISO-8601 string OR a Unix-epoch-ms integer;
                // absent / null = never expires (0). Non-string/int → 0 (never).
                int64_t expires_at_ms = 0;
                if (body.contains("expires_at") && body["expires_at"].is_number_integer()) {
                    expires_at_ms = body["expires_at"].get<int64_t>();
                }
                auto r = keys.CreateApiKey(rctx.auth.user_id, body["name"].get<std::string>(),
                                           expires_at_ms);
                if (!r.ok()) {
                    WriteJsonError(res, r.status(), rctx.request_id);
                    return;
                }
                const auto& created = r.value();
                nlohmann::json out;
                out["id"]         = created.info.id;
                out["key"]        = created.plaintext;  // ONLY time the plaintext is returned
                out["name"]       = created.info.name;
                out["user_id"]    = created.info.user_id;
                out["created_at"] = created.info.created_at;
                out["expires_at"] = created.info.expires_at == 0
                                        ? nlohmann::json(nullptr)
                                        : nlohmann::json(created.info.expires_at);
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }));

    // GET /api/v1/auth/api-keys — list the caller's keys (never the plaintext).
    server.Get("/api/v1/auth/api-keys",
        WithAuth(auth, kPermAdmin,
            [&keys](const httplib::Request& /*req*/, httplib::Response& res,
                    const RequestContext& rctx) {
                auto r = keys.ListApiKeys(rctx.auth.user_id);
                if (!r.ok()) {
                    WriteJsonError(res, r.status(), rctx.request_id);
                    return;
                }
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& k : r.value()) arr.push_back(ApiKeyInfoToJson(k));
                WriteJsonResponse(res, 200, arr, rctx.request_id);
            }));

    // DELETE /api/v1/auth/api-keys/{key_id} — revoke (soft delete, revoked_at).
    server.Delete(R"(/api/v1/auth/api-keys/([^/]+))",
        WithAuth(auth, kPermAdmin,
            [&keys](const httplib::Request& req, httplib::Response& res,
                    const RequestContext& rctx) {
                const std::string key_id = req.matches.size() > 1 ? req.matches[1].str() : "";
                Status s = keys.RevokeApiKey(key_id);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                nlohmann::json out;
                out["revoked"] = true;
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }));
}

}  // namespace cortrix
