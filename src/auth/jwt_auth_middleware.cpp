#include "cortrix/auth/jwt_auth_middleware.h"

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_error.h"

namespace cortrix::auth {

std::string JwtAuthMiddleware::ExtractBearer(const std::string& authorization) {
    static const std::string kPrefix = "Bearer ";
    if (authorization.size() > kPrefix.size() &&
        authorization.compare(0, kPrefix.size(), kPrefix) == 0) {
        return authorization.substr(kPrefix.size());
    }
    return "";
}

bool JwtAuthMiddleware::IsApiKeyToken(const std::string& bearer) {
    static const std::string kApiKeyPrefix = "cortrix_sk_";
    return bearer.compare(0, kApiKeyPrefix.size(), kApiKeyPrefix) == 0;
}

Result<AuthContext> JwtAuthMiddleware::AuthenticateHeaders(
    const std::string& authorization, const std::string& x_api_key) const {
    if (auth_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "middleware not initialized");
    }

    const std::string bearer = ExtractBearer(authorization);

    // API-Key path (step 1): a `cortrix_sk_*` bearer or an X-API-Key
    // header. The API-Key validation itself is S6 — here we report it is not the
    // JWT path so the caller (D3.5 wiring) routes it to ApiKeyService.
    if (IsApiKeyToken(bearer) || !x_api_key.empty()) {
        return AuthStatus(AuthErrorCode::kNotFound, "api-key path handled by S6 ApiKeyService");
    }

    // No credentials at all → 401 ("none → CX_ERR_UNAUTHORIZED").
    if (bearer.empty()) {
        return AuthStatus(AuthErrorCode::kUnauthorized,
                          "missing Authorization bearer token or X-API-Key");
    }

    // JWT path (§4.3 step 2-4): validate + return AuthContext. AuthService maps
    // expiry/blacklist/signature to the right CX_ERR_AUTH_* codes.
    return auth_->ValidateAccessToken(bearer);
}

std::string BuildMeResponseJson(const UserInfo& user) {
    nlohmann::json j;
    j["id"] = user.id;
    j["email"] = user.email;
    j["display_name"] = user.display_name;
    j["email_verified"] = user.email_verified;
    j["created_at"] = user.created_at;
    nlohmann::json tenants = nlohmann::json::array();
    for (const TenantRef& t : user.tenants) {
        tenants.push_back({{"id", t.id}, {"name", t.name}, {"role", t.role}});
    }
    j["tenants"] = tenants;  // empty until the tenant service is wired
    return j.dump();
}

}  // namespace cortrix::auth
