#pragma once
#include <string>

#include "cortrix/auth/auth_service.h"
#include "cortrix/auth/auth_token.h"
#include "cortrix/common/result.h"

namespace cortrix::auth {

/// The authentication decision for a Cloud request,
/// as a pure, httplib-independent unit so it is fully testable standalone. It
/// turns the request's auth headers into either an AuthContext or an
/// Agent-friendly error, by delegating JWT validation to AuthService.
///
/// 🚩 D3.5 (route wiring): mounting this on the real httplib server — the
/// `Authenticate(httplib::Request) -> optional<Response>` wrapper, protecting the
/// existing routes, the CE-vs-Cloud edition switch (CE keeps the MVP ApiKeyAuth
/// path untouched), and the Bearer-cortrix_sk_* API-Key branch (S6) — is
/// cross-Feature server wiring and lands at D3.5. This class is the logic those
/// will call.
class JwtAuthMiddleware {
public:
    explicit JwtAuthMiddleware(AuthService* auth) : auth_(auth) {}

    /// Decide auth from the raw header values (step 1-3):
    ///   - "Authorization: Bearer <jwt>"  → AuthService::ValidateAccessToken;
    ///   - "Authorization: Bearer cortrix_sk_..." OR non-empty `x_api_key`
    ///       → API-Key path → returns CX_ERR_NOT_FOUND here (the API-Key branch
    ///       is S6; this method handles the JWT path and reports "not handled");
    ///   - neither present → CX_ERR_UNAUTHORIZED (missing credentials).
    /// On a valid JWT, returns the AuthContext (§2.15).
    Result<AuthContext> AuthenticateHeaders(const std::string& authorization,
                                            const std::string& x_api_key) const;

    /// Extract the bearer token from an "Authorization: Bearer <x>" header value,
    /// or "" if the header is absent / not a Bearer scheme. Exposed for tests.
    static std::string ExtractBearer(const std::string& authorization);

    /// True if a bearer value looks like a Cortrix API Key (`cortrix_sk_` prefix),
    /// i.e. should go down the API-Key path (S6) rather than JWT decode.
    static bool IsApiKeyToken(const std::string& bearer);

private:
    AuthService* auth_;
};

/// Build the `GET /api/v1/auth/me` JSON body from a UserInfo. Pure
/// (no IO) so it is unit-testable; the route that calls it is D3.5.
std::string BuildMeResponseJson(const UserInfo& user);

}  // namespace cortrix::auth
