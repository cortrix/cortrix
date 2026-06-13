#pragma once

namespace httplib { class Server; }

namespace cortrix {
class ApiKeyAuth;
class IGlobalConfig;

/// [D3.5 r2 · Wave P · P4] Register the F48 §6.3 agent LLM config admin API:
///   GET /api/v1/system/agent_llm_config  -- read current config (api_key masked)
///   PUT /api/v1/system/agent_llm_config  -- update config (admin only)
///
/// Backed by IGlobalConfig.Get/SetAgentLlmConfig (api_key encrypted at rest). The
/// path is NOT under the AdminGuard /api/v1/admin/* prefix, so PUT enforces admin
/// in-handler (WithAuth + AuthContext.is_admin → CX_ERR_AUTH_ADMIN_REQUIRED). GET
/// is read-permission. `config` must outlive `server`.
void RegisterSystemConfigRoutes(httplib::Server& server, IGlobalConfig& config,
                                ApiKeyAuth& auth);

}  // namespace cortrix
