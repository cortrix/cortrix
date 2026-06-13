#include "cortrix/server/routes/system_config_routes.h"

#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/common/agent_llm_config_codec.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/server/http_server.h"

namespace cortrix {

namespace {

using cortrix::auth::AuthErrorCode;
using cortrix::auth::MakeAuthError;

// F48 §6.2 / §7 — the 6 allowed providers. An unknown provider is rejected with
// CX_ERR_INVALID_REQUEST (the field-level validation the Agent-first PUT owes).
const std::set<std::string>& AllowedProviders() {
    static const std::set<std::string> kProviders = {
        "openai", "claude", "ollama", "glm", "deepseek", "mock"};
    return kProviders;
}

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

// §6.3 GET shape — api_key masked (never the plaintext). provider "" surfaces as
// the JSON null sentinel so an Agent sees "unconfigured" explicitly.
nlohmann::json ToReadJson(const AgentLlmConfig& cfg) {
    nlohmann::json j;
    j["provider"]    = cfg.provider.empty() ? nlohmann::json(nullptr)
                                            : nlohmann::json(cfg.provider);
    j["api_key"]     = cfg.api_key.empty()
                           ? nlohmann::json(nullptr)
                           : nlohmann::json(agent_llm_codec::MaskApiKey(cfg.api_key));
    j["model"]       = cfg.model;
    j["base_url"]    = cfg.base_url;
    j["max_tokens"]  = cfg.max_tokens;
    j["temperature"] = cfg.temperature;
    return j;
}

}  // namespace

void RegisterSystemConfigRoutes(httplib::Server& server, IGlobalConfig& config,
                                ApiKeyAuth& auth) {
    // GET /api/v1/system/agent_llm_config — read (api_key masked). Read permission.
    server.Get("/api/v1/system/agent_llm_config",
        WithAuth(auth, kPermRead,
            [&config](const httplib::Request& /*req*/, httplib::Response& res,
                      const RequestContext& rctx) {
                WriteJsonResponse(res, 200, ToReadJson(config.GetAgentLlmConfig()),
                                  rctx.request_id);
            }));

    // PUT /api/v1/system/agent_llm_config — update (admin only). The path is not
    // under AdminGuard, so admin is enforced here. A field omitted from the body
    // keeps its current value (merge semantics); api_key="" leaves the stored key
    // unchanged (a key is never cleared by an omitted/empty field — Agent-friendly).
    server.Put("/api/v1/system/agent_llm_config",
        WithAuth(auth, kPermWrite,
            [&config](const httplib::Request& req, httplib::Response& res,
                      const RequestContext& rctx) {
                if (!rctx.auth.is_admin()) {
                    WriteAuthError(res, AuthErrorCode::kAdminRequired,
                                   "admin role required to update agent_llm config",
                                   rctx.request_id, {{"required_role", "admin"}});
                    return;
                }
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteAuthError(res, AuthErrorCode::kInvalidRequest,
                                   "request body must be a JSON object", rctx.request_id);
                    return;
                }
                if (!body.is_object()) {
                    WriteAuthError(res, AuthErrorCode::kInvalidRequest,
                                   "request body must be a JSON object", rctx.request_id);
                    return;
                }

                // Merge onto the current config (partial update).
                AgentLlmConfig cfg = config.GetAgentLlmConfig();
                if (body.contains("provider") && body["provider"].is_string()) {
                    const std::string p = body["provider"].get<std::string>();
                    if (AllowedProviders().count(p) == 0) {
                        WriteAuthError(res, AuthErrorCode::kInvalidRequest,
                                       "unsupported provider '" + p + "'", rctx.request_id,
                                       {{"field", "provider"}, {"value", p}});
                        return;
                    }
                    cfg.provider = p;
                }
                if (body.contains("api_key") && body["api_key"].is_string()) {
                    const std::string k = body["api_key"].get<std::string>();
                    if (!k.empty()) cfg.api_key = k;  // empty = keep current (never clear)
                }
                if (body.contains("model") && body["model"].is_string())
                    cfg.model = body["model"].get<std::string>();
                if (body.contains("base_url") && body["base_url"].is_string())
                    cfg.base_url = body["base_url"].get<std::string>();
                if (body.contains("max_tokens") && body["max_tokens"].is_number_integer())
                    cfg.max_tokens = body["max_tokens"].get<int>();
                if (body.contains("temperature") && body["temperature"].is_number())
                    cfg.temperature = body["temperature"].get<double>();

                config.SetAgentLlmConfig(cfg);
                // Echo back the masked, persisted config (§6.3 read shape).
                WriteJsonResponse(res, 200, ToReadJson(config.GetAgentLlmConfig()),
                                  rctx.request_id);
            }));
}

}  // namespace cortrix
