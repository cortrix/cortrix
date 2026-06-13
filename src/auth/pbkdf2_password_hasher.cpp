#include "cortrix/auth/pbkdf2_password_hasher.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <string>
#include <vector>

#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/jwt_utils.h"  // reuse Base64UrlEncode/Decode (no-pad base64url)

namespace cortrix::auth {

namespace {

constexpr int kSaltBytes = 16;
constexpr int kDkBytes = 32;  // SHA-256 output size

// PBKDF2-HMAC-SHA256(password, salt, iters) → kDkBytes derived key.
Result<std::string> Pbkdf2(const std::string& password, const std::string& salt,
                           int iterations) {
    std::string dk(kDkBytes, '\0');
    const int rc = PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
        iterations, EVP_sha256(), kDkBytes,
        reinterpret_cast<unsigned char*>(dk.data()));
    if (rc != 1) {
        return AuthStatus(AuthErrorCode::kInternalError, "PBKDF2 derivation failed");
    }
    return dk;
}

// Split `s` on '$' into segments.
std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(delim, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

}  // namespace

Result<std::string> Pbkdf2PasswordHasher::Hash(const std::string& password) {
    std::string salt(kSaltBytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), kSaltBytes) != 1) {
        return AuthStatus(AuthErrorCode::kInternalError, "salt generation failed");
    }
    Result<std::string> dk = Pbkdf2(password, salt, iterations_);
    if (!dk.ok()) return dk.status();

    // pbkdf2$sha256$<iters>$<b64salt>$<b64dk>  (NOT $2b$ — placeholder marker)
    return "pbkdf2$sha256$" + std::to_string(iterations_) + "$" +
           JwtCodec::Base64UrlEncode(salt) + "$" + JwtCodec::Base64UrlEncode(dk.value());
}

Result<bool> Pbkdf2PasswordHasher::Verify(const std::string& password,
                                          const std::string& hash) {
    // Parse the self-describing string. A malformed hash is an error (not "false")
    // so a storage corruption is distinguishable from a wrong password.
    const std::vector<std::string> parts = Split(hash, '$');
    if (parts.size() != 5 || parts[0] != "pbkdf2" || parts[1] != "sha256") {
        return AuthStatus(AuthErrorCode::kInternalError, "malformed pbkdf2 hash");
    }
    int iters = 0;
    try {
        iters = std::stoi(parts[2]);
    } catch (...) {
        return AuthStatus(AuthErrorCode::kInternalError, "bad pbkdf2 iteration count");
    }
    if (iters <= 0) {
        return AuthStatus(AuthErrorCode::kInternalError, "bad pbkdf2 iteration count");
    }
    Result<std::string> salt = JwtCodec::Base64UrlDecode(parts[3]);
    if (!salt.ok()) return AuthStatus(AuthErrorCode::kInternalError, "bad pbkdf2 salt");
    Result<std::string> stored_dk = JwtCodec::Base64UrlDecode(parts[4]);
    if (!stored_dk.ok()) return AuthStatus(AuthErrorCode::kInternalError, "bad pbkdf2 dk");

    // Re-derive with the stored salt + iterations and constant-time compare.
    Result<std::string> dk = Pbkdf2(password, salt.value(), iters);
    if (!dk.ok()) return dk.status();
    if (dk.value().size() != stored_dk.value().size()) return false;
    return CRYPTO_memcmp(dk.value().data(), stored_dk.value().data(),
                         dk.value().size()) == 0;
}

}  // namespace cortrix::auth
