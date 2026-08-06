#include <cstdint>
#include "cortrix/server/routes/auth_routes.h"

#include <cstdlib>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/admin_users_service.h"
#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/bootstrap_handler.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/auth/jwt_secret_service.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/server/http_server.h"

namespace cortrix {

namespace {

using cortrix::auth::AdminUserListFilter;
using cortrix::auth::AdminUserRecord;
using cortrix::auth::AdminUsersService;
using cortrix::auth::ApiKeyInfo;
using cortrix::auth::ApiKeyService;
using cortrix::auth::AuthErrorCode;
using cortrix::auth::BootstrapHandler;
using cortrix::auth::JwtSecretService;
using cortrix::auth::MakeAuthError;

// Write the GEN-Agent 4-field auth error body for `code` with the matching
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

// Serialize one ApiKeyInfo to the list shape (never the plaintext). 0 / ""
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

// Serialize one AdminUserRecord to the UserRecord shape. created_at is
// surfaced as Unix seconds (the frontend treats it as an opaque timestamp).
nlohmann::json AdminUserToJson(const AdminUserRecord& u) {
    nlohmann::json j;
    j["id"]             = u.id;
    j["email"]          = u.email;
    j["display_name"]   = u.display_name;
    j["role"]           = u.role;
    j["status"]         = u.status;
    j["email_verified"] = u.email_verified;
    j["created_at"]     = u.created_at;
    return j;
}

// Recover the AuthErrorCode an AdminUsersService Status carries (the CX_ERR_*
// token is the leading word of the message). Only the codes the service emits
// are matched; anything else falls back to a generic 503/internal mapping.
AuthErrorCode AdminUserErrorCode(const Status& s) {
    const std::string& m = s.message();
    if (m.rfind("CX_ERR_USER_NOT_FOUND", 0) == 0)    return AuthErrorCode::kUserNotFound;
    if (m.rfind("CX_ERR_USER_EMAIL_EXISTS", 0) == 0) return AuthErrorCode::kUserEmailExists;
    if (m.rfind("CX_ERR_USER_VALIDATION", 0) == 0)   return AuthErrorCode::kUserValidation;
    if (m.rfind("CX_ERR_INVALID_REQUEST", 0) == 0)   return AuthErrorCode::kInvalidRequest;
    if (m.rfind("CX_ERR_INTERNAL_ERROR", 0) == 0)    return AuthErrorCode::kInternalError;
    return AuthErrorCode::kServiceUnavailable;
}

// Write the GEN-Agent error body for an AdminUsersService failure. Mirrors
// WriteAuthError but pins the HTTP statuses: 404 / 409 / 422 / 400.
// (CX_ERR_USER_VALIDATION is 422 per — distinct from the coarse
// kInvalidArgument→400 Status mapping, so it is set explicitly here.)
void WriteAdminUserError(httplib::Response& res, const Status& s,
                         const std::string& request_id) {
    const AuthErrorCode code = AdminUserErrorCode(s);
    WriteAuthError(res, code, s.message(), request_id);
    if (code == AuthErrorCode::kUserValidation) res.status = 422;
}

// Best-effort operation_log write for the admin/users mutations (the
// 4 mutating endpoints log user.created / user.updated / user.disabled /
// user.enabled). `logger` may be null (the route can be registered without an
// observability module in tests) — a null logger is a silent no-op.
void LogAdminUserAction(observability::IOperationLogger* logger,
                        const std::string& actor_user_id, const std::string& action,
                        const std::string& target_user_id) {
    if (logger == nullptr) return;
    observability::OperationLogEntry e;
    e.user_id = actor_user_id;
    e.action = action;                 // user.created / user.updated / ...
    e.resource_type = "user";
    e.resource_id = target_user_id;
    logger->Log(e);
}

}  // namespace

void RegisterBootstrapRoutes(httplib::Server& server, BootstrapHandler& handler) {
    // GET /api/v1/admin/bootstrap?token=<t> — first-run browser flow.
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
    //. Same token + same Consume logic; only the rendering differs.
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
    // POST /api/v1/auth/api-keys — create a key for the caller. The
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

void RegisterAuthSessionRoute(httplib::Server& server, ApiKeyAuth& auth) {
    // GET /api/v1/auth/me — CE Web UI session probe (the web UI / quick-experience mode).
    //
    // The JWT email/password /auth/me is a cloud-enterprise endpoint, absent in CE (an
    // un-shimmed CE would 404 here). The self-hosted Web UI probes this on startup to
    // decide isAuthenticated. When auth is DISABLED (the V1 default — out-of-the-box
    // "quick experience"), report a synthetic single-operator identity so the probe
    // authenticates and the app shell renders; otherwise the UI dead-ends on a login
    // wall whose email/password backend is itself cloud-enterprise. When auth is
    // ENABLED, return 401 so the UI shows login (the full CE auth-on Web UI login flow
    // is a V1.5 multi-user item — hub PHASE2_BACKLOG TD-CE-WEBUI-AUTH-ON).
    //
    // Not WithAuth-wrapped by design: this IS the pre-auth probe; its only gate is
    // reading auth.enabled().
    server.Get("/api/v1/auth/me",
        [&auth](const httplib::Request& /*req*/, httplib::Response& res) {
            const std::string request_id = GenerateRequestId();
            if (auth.enabled()) {
                WriteAuthError(res, cortrix::auth::AuthErrorCode::kUnauthorized,
                               "authentication required", request_id);
                return;
            }
            // auth disabled → single local operator (full access; the backend already
            // grants admin to every request in no-auth mode).
            nlohmann::json body;
            body["id"]             = "operator";
            body["email"]          = "operator@localhost";
            body["display_name"]   = "Operator";
            body["email_verified"] = true;
            body["role"]           = "admin";
            body["edition"]        = "ce";
            body["auth_disabled"]  = true;
            WriteJsonResponse(res, 200, body, request_id);
        });
}

void RegisterAdminUsersRoutes(httplib::Server& server, AdminUsersService& users,
                              ApiKeyAuth& auth,
                              observability::IOperationLogger* logger) {
    // GET /api/v1/admin/users — list with q / status / role / page / limit.
    server.Get("/api/v1/admin/users",
        WithAuth(auth, kPermAdmin,
            [&users](const httplib::Request& req, httplib::Response& res,
                     const RequestContext& rctx) {
                AdminUserListFilter f;
                if (req.has_param("q"))      f.q = req.get_param_value("q");
                if (req.has_param("status")) f.status = req.get_param_value("status");
                if (req.has_param("role"))   f.role = req.get_param_value("role");
                if (req.has_param("page"))   f.page = std::atoi(req.get_param_value("page").c_str());
                if (req.has_param("limit"))  f.limit = std::atoi(req.get_param_value("limit").c_str());
                auto r = users.List(f);
                if (!r.ok()) {
                    WriteAdminUserError(res, r.status(), rctx.request_id);
                    return;
                }
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& u : r.value().users) arr.push_back(AdminUserToJson(u));
                nlohmann::json out;
                out["users"] = arr;
                out["total"] = r.value().total;
                out["page"]  = r.value().page;
                out["limit"] = r.value().limit;
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }));

    // POST /api/v1/admin/users — invite-mode create.
    server.Post("/api/v1/admin/users",
        WithAuth(auth, kPermAdmin,
            [&users, logger](const httplib::Request& req, httplib::Response& res,
                             const RequestContext& rctx) {
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteAuthError(res, AuthErrorCode::kUserValidation,
                                   "request body must be a JSON object", rctx.request_id);
                    res.status = 422;
                    return;
                }
                if (!body.is_object()) {
                    WriteAuthError(res, AuthErrorCode::kUserValidation,
                                   "request body must be a JSON object", rctx.request_id);
                    res.status = 422;
                    return;
                }
                auto str = [&](const char* k) -> std::string {
                    return (body.contains(k) && body[k].is_string())
                               ? body[k].get<std::string>() : std::string();
                };
                auto r = users.Create(str("email"), str("password"), str("role"),
                                      str("display_name"));
                if (!r.ok()) {
                    WriteAdminUserError(res, r.status(), rctx.request_id);
                    return;
                }
                LogAdminUserAction(logger, rctx.auth.user_id, "user.created", r.value().id);
                nlohmann::json out;
                out["user"] = AdminUserToJson(r.value());
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }));

    // PATCH /api/v1/admin/users/:id — update display_name / role / email_verified.
    server.Patch(R"(/api/v1/admin/users/([^/]+))",
        WithAuth(auth, kPermAdmin,
            [&users, logger](const httplib::Request& req, httplib::Response& res,
                             const RequestContext& rctx) {
                const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteAuthError(res, AuthErrorCode::kUserValidation,
                                   "request body must be a JSON object", rctx.request_id);
                    res.status = 422;
                    return;
                }
                std::optional<std::string> display_name, role;
                std::optional<bool> email_verified;
                if (body.is_object()) {
                    if (body.contains("display_name") && body["display_name"].is_string())
                        display_name = body["display_name"].get<std::string>();
                    if (body.contains("role") && body["role"].is_string())
                        role = body["role"].get<std::string>();
                    if (body.contains("email_verified") && body["email_verified"].is_boolean())
                        email_verified = body["email_verified"].get<bool>();
                }
                auto r = users.Update(id, display_name, role, email_verified);
                if (!r.ok()) {
                    WriteAdminUserError(res, r.status(), rctx.request_id);
                    return;
                }
                LogAdminUserAction(logger, rctx.auth.user_id, "user.updated", id);
                nlohmann::json out;
                out["user"] = AdminUserToJson(r.value());
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }));

    // POST /api/v1/admin/users/:id/disable — active → disabled (soft).
    server.Post(R"(/api/v1/admin/users/([^/]+)/disable)",
        WithAuth(auth, kPermAdmin,
            [&users, logger](const httplib::Request& req, httplib::Response& res,
                             const RequestContext& rctx) {
                const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto r = users.Disable(id);
                if (!r.ok()) {
                    WriteAdminUserError(res, r.status(), rctx.request_id);
                    return;
                }
                LogAdminUserAction(logger, rctx.auth.user_id, "user.disabled", id);
                nlohmann::json out;
                out["user"] = AdminUserToJson(r.value());
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }));

    // POST /api/v1/admin/users/:id/enable — disabled → active.
    server.Post(R"(/api/v1/admin/users/([^/]+)/enable)",
        WithAuth(auth, kPermAdmin,
            [&users, logger](const httplib::Request& req, httplib::Response& res,
                             const RequestContext& rctx) {
                const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto r = users.Enable(id);
                if (!r.ok()) {
                    WriteAdminUserError(res, r.status(), rctx.request_id);
                    return;
                }
                LogAdminUserAction(logger, rctx.auth.user_id, "user.enabled", id);
                nlohmann::json out;
                out["user"] = AdminUserToJson(r.value());
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }));
}

void RegisterJwtRotateRoute(httplib::Server& server, JwtSecretService& jwt,
                            ApiKeyAuth& auth) {
    // POST /api/v1/admin/auth/rotate-jwt-secret — topic 1.1 C. Rotates the
    // HS256 signing secret (current → prev 24h window + new current) and returns the
    // Agent-friendly RotationResult. kPermAdmin-gated; AdminGuard adds the loopback
    // IP layer ("Admin Endpoint dual-layer protection").
    server.Post("/api/v1/admin/auth/rotate-jwt-secret",
        WithAuth(auth, kPermAdmin,
            [&jwt](const httplib::Request& /*req*/, httplib::Response& res,
                   const RequestContext& rctx) {
                auto r = jwt.RotateJwtSecret();
                if (!r.ok()) {
                    WriteJsonError(res, r.status(), rctx.request_id);
                    return;
                }
                const auto& rot = r.value();
                // response shape: rotated / new_key_id / prev_key_id /
                // prev_expires_at + the Agent-friendly warning.
                nlohmann::json out;
                out["rotated"]      = true;
                out["new_key_id"]   = rot.new_key_id;
                out["prev_key_id"]  = rot.prev_key_id;
                out["prev_expires_at"] = rot.prev_expires_at_ms;
                out["warning"] =
                    "Prev key expires in 24h; ensure all clients refresh before then";
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }));
}

}  // namespace cortrix
