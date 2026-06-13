#pragma once
#include <string>

namespace cortrix::auth {

/// Decoded identity of an authenticated principal (P08 §2.15). This is the P08
/// (Cloud JWT) auth context — distinct from the MVP root-namespace
/// `cortrix::AuthContext` (API-Key, tenant/permissions bitmask). The two coexist
/// during D3; unifying them is part of the middleware wiring (D3.5).
struct AuthContext {
    std::string user_id;    ///< "usr_..."  (= JWT sub)
    std::string email;
    std::string tenant_id;  ///< active tenant ("tenant-<uuid>")
    std::string role;       ///< owner | admin | member | viewer
    std::string session_id; ///< "sess_..." (= JWT sid; OBSERVABILITY §5.3)
};

/// Token pair returned by Login (P08 §2.15 / §2.3).
struct AuthTokenPair {
    std::string access_token;
    std::string refresh_token;
    int expires_in = 0;     ///< access_token lifetime (seconds)
};

}  // namespace cortrix::auth
