#include <cstdint>
#include "cortrix/auth/jwt_utils.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cortrix::auth {

namespace {

// Standard base64 alphabets. We encode with the standard table then translate to
// the URL-safe alphabet and strip '=' padding (RFC 7515). Decoding reverses it.
const char kStdAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int DecodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;  // accept both std and url-safe
    if (c == '/' || c == '_') return 63;
    return -1;
}

}  // namespace

std::string JwtCodec::Base64UrlEncode(const std::string& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    const auto* d = reinterpret_cast<const unsigned char*>(data.data());
    while (i + 3 <= data.size()) {
        const unsigned n = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        out.push_back(kStdAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kStdAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kStdAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kStdAlphabet[n & 0x3F]);
        i += 3;
    }
    const std::size_t rem = data.size() - i;
    if (rem == 1) {
        const unsigned n = d[i] << 16;
        out.push_back(kStdAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kStdAlphabet[(n >> 12) & 0x3F]);
    } else if (rem == 2) {
        const unsigned n = (d[i] << 16) | (d[i + 1] << 8);
        out.push_back(kStdAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kStdAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kStdAlphabet[(n >> 6) & 0x3F]);
    }
    // Translate to URL-safe alphabet (no '+'/'/' here since kStdAlphabet uses them).
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return out;  // no padding (RFC 7515)
}

Result<std::string> JwtCodec::Base64UrlDecode(const std::string& in) {
    // Reject any char outside the alphabet (padding '=' allowed but ignored).
    std::vector<int> vals;
    vals.reserve(in.size());
    for (char c : in) {
        if (c == '=') continue;
        const int v = DecodeChar(c);
        if (v < 0) return AuthStatus(AuthErrorCode::kUnauthorized, "invalid base64url char");
        vals.push_back(v);
    }
    std::string out;
    out.reserve((vals.size() * 3) / 4);
    std::size_t i = 0;
    while (i + 4 <= vals.size()) {
        const unsigned n = (vals[i] << 18) | (vals[i + 1] << 12) | (vals[i + 2] << 6) | vals[i + 3];
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
        i += 4;
    }
    const std::size_t rem = vals.size() - i;
    if (rem == 2) {
        const unsigned n = (vals[i] << 18) | (vals[i + 1] << 12);
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
    } else if (rem == 3) {
        const unsigned n = (vals[i] << 18) | (vals[i + 1] << 12) | (vals[i + 2] << 6);
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
    } else if (rem == 1) {
        return AuthStatus(AuthErrorCode::kUnauthorized, "invalid base64url length");
    }
    return out;
}

std::string JwtCodec::HmacSha256(const std::string& secret, const std::string& msg) {
    unsigned char mac[SHA256_DIGEST_LENGTH];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         mac, &len);
    return std::string(reinterpret_cast<char*>(mac), len);
}

Result<std::string> JwtCodec::Encode(const JwtPayload& p, const std::string& secret) {
    // Fixed header.
    static const std::string kHeaderJson = R"({"alg":"HS256","typ":"JWT"})";

    nlohmann::json claims;
    claims["sub"] = p.sub;
    claims["sid"] = p.sid;
    claims["iat"] = p.iat;
    claims["exp"] = p.exp;
    claims["iss"] = p.iss;
    claims["aud"] = p.aud;
    if (p.is_refresh()) {
        claims["type"] = p.type;       // "refresh"
        claims["jti"] = p.jti;
    } else {
        // Access token — 9 fields (topic 3.3): + email/tenant_id/role.
        claims["email"] = p.email;
        claims["tenant_id"] = p.tenant_id;
        claims["role"] = p.role;
    }

    const std::string signing_input =
        Base64UrlEncode(kHeaderJson) + "." + Base64UrlEncode(claims.dump());
    const std::string sig = HmacSha256(secret, signing_input);
    return signing_input + "." + Base64UrlEncode(sig);
}

Result<JwtPayload> JwtCodec::Decode(const std::string& token,
                                    const std::vector<std::string>& accept_secrets,
                                    int64_t now_sec) {
    // 1) split into exactly 3 segments.
    const std::size_t dot1 = token.find('.');
    if (dot1 == std::string::npos) {
        return AuthStatus(AuthErrorCode::kUnauthorized, "malformed jwt (no segments)");
    }
    const std::size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos || token.find('.', dot2 + 1) != std::string::npos) {
        return AuthStatus(AuthErrorCode::kUnauthorized, "malformed jwt (segment count)");
    }
    const std::string signing_input = token.substr(0, dot2);
    const std::string header_b64 = token.substr(0, dot1);
    const std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    const std::string sig_b64 = token.substr(dot2 + 1);

    // 2) header alg == HS256.
    Result<std::string> header_json = Base64UrlDecode(header_b64);
    if (!header_json.ok()) return header_json.status();
    try {
        nlohmann::json h = nlohmann::json::parse(header_json.value());
        if (!h.contains("alg") || h["alg"] != "HS256") {
            return AuthStatus(AuthErrorCode::kUnauthorized, "unsupported jwt alg");
        }
    } catch (const nlohmann::json::exception&) {
        return AuthStatus(AuthErrorCode::kUnauthorized, "bad jwt header json");
    }

    // 3) signature: recompute and constant-time compare against each accepted
    //    secret (dual-key window in S7). Decode the provided sig once.
    Result<std::string> provided_sig = Base64UrlDecode(sig_b64);
    if (!provided_sig.ok()) return provided_sig.status();
    bool sig_ok = false;
    for (const std::string& secret : accept_secrets) {
        const std::string expect = HmacSha256(secret, signing_input);
        if (expect.size() == provided_sig.value().size() &&
            CRYPTO_memcmp(expect.data(), provided_sig.value().data(), expect.size()) == 0) {
            sig_ok = true;
            break;
        }
    }
    if (!sig_ok) {
        return AuthStatus(AuthErrorCode::kUnauthorized, "jwt signature mismatch");
    }

    // 4) parse claims.
    Result<std::string> payload_json = Base64UrlDecode(payload_b64);
    if (!payload_json.ok()) return payload_json.status();
    JwtPayload p;
    try {
        nlohmann::json c = nlohmann::json::parse(payload_json.value());
        p.sub = c.value("sub", "");
        p.sid = c.value("sid", "");
        p.iat = c.value("iat", static_cast<int64_t>(0));
        p.exp = c.value("exp", static_cast<int64_t>(0));
        p.iss = c.value("iss", "");
        p.aud = c.value("aud", "");
        p.email = c.value("email", "");
        p.tenant_id = c.value("tenant_id", "");
        p.role = c.value("role", "");
        p.type = c.value("type", "");
        p.jti = c.value("jti", "");
    } catch (const nlohmann::json::exception&) {
        return AuthStatus(AuthErrorCode::kUnauthorized, "bad jwt payload json");
    }

    // 5) expiry (distinct CX_ERR_AUTH_TOKEN_EXPIRED).
    if (p.exp != 0 && now_sec >= p.exp) {
        return AuthStatus(AuthErrorCode::kTokenExpired, "jwt expired");
    }
    return p;
}

}  // namespace cortrix::auth
