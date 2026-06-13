#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/auth/auth_error.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::auth {

/// JWT claims for the two Cortrix token kinds (P08 §2.10). Modeled as one struct
/// covering both; `is_refresh()` distinguishes them. The access token carries 9
/// fields (topic 3.3 C, benchmarked against InsForge); the refresh token carries sub/sid/type/jti.
struct JwtPayload {
    // Common.
    std::string sub;            ///< user id ("usr_...")
    std::string sid;            ///< session id ("sess_...")
    int64_t iat = 0;            ///< issued-at (unix seconds)
    int64_t exp = 0;            ///< expiry (unix seconds)
    std::string iss = "cortrix-auth";  ///< topic 3.3 — single-value placeholder
    std::string aud = "cortrix";       ///< topic 3.3 — single-value placeholder

    // Access-token-only (empty on a refresh token).
    std::string email;
    std::string tenant_id;
    std::string role;           ///< owner | admin | member | viewer

    // Refresh-token-only.
    std::string type;           ///< "refresh" on a refresh token; "" (access) otherwise
    std::string jti;            ///< refresh token unique id ("ref_...")

    bool is_refresh() const { return type == "refresh"; }
};

/// HS256 (HMAC-SHA256) JWT codec built on OpenSSL (team-lead approved 2026-05-31:
/// HS256 == HMAC-SHA256, OpenSSL-native, offline, no third-party dep; not a
/// re-implemented crypto primitive). Header is fixed `{"alg":"HS256","typ":"JWT"}`.
class JwtCodec {
public:
    /// Build a signed compact JWS (`base64url(header).base64url(payload).base64url(sig)`)
    /// from `payload`, signed with `secret` (the raw HS256 key). Serializes only the
    /// fields relevant to the token kind (access: 9 fields; refresh: sub/sid/type/jti
    /// + iat/exp/iss/aud).
    static Result<std::string> Encode(const JwtPayload& payload, const std::string& secret);

    /// Verify + decode `token`. Tries each secret in `accept_secrets` in order
    /// (S3 passes one; S7's dual-key window passes {current, prev}). Checks:
    ///   - well-formed 3-segment structure + base64url + JSON;
    ///   - alg == HS256;
    ///   - HMAC signature matches one accepted secret (constant-time compare);
    ///   - exp > now (expiry).
    /// Errors map to §5.1: bad structure/sig → CX_ERR_UNAUTHORIZED;
    /// expired → CX_ERR_AUTH_TOKEN_EXPIRED.
    static Result<JwtPayload> Decode(const std::string& token,
                                     const std::vector<std::string>& accept_secrets,
                                     int64_t now_sec);

    // ---- base64url (RFC 7515 §2 — no padding, '-'/'_' alphabet). Exposed for tests.
    static std::string Base64UrlEncode(const std::string& data);
    static Result<std::string> Base64UrlDecode(const std::string& in);

    /// Raw HMAC-SHA256(secret, msg) → 32 raw bytes (OpenSSL HMAC). Exposed for tests.
    static std::string HmacSha256(const std::string& secret, const std::string& msg);
};

}  // namespace cortrix::auth
