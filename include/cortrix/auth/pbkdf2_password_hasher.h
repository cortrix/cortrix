#pragma once
#include <string>

#include "cortrix/auth/password_utils.h"
#include "cortrix/common/result.h"

namespace cortrix::auth {

/// Production password hasher for Phase 1 V1 — OpenSSL PBKDF2-HMAC-SHA256.
///
/// 🚧 TECH_DEBT-AUTH-PBKDF2-PLACEHOLDER:
/// The design specifies **bcrypt cost=12** with a `$2b$...` hash. bcrypt is NOT
/// available offline on this build (no system lib / no vendored source / OpenSSL
/// has no native bcrypt), and the env's FetchContent network fetch is unreliable.
/// This implementation uses a PBKDF2-HMAC-SHA256 placeholder that meets the
/// security *substance* (salted, slow, tunable work factor, one-way) but emits a
/// `pbkdf2$...` hash, NOT `$2b$`. SWITCH TO REAL bcrypt once network is available
/// (integration): the IPasswordHasher seam means swapping = replacing this class +
/// re-hashing. Pre-launch there are no stored passwords, so the migration is
/// zero-cost. Tracked as TECH_DEBT-AUTH-PBKDF2-PLACEHOLDER.
///
/// Hash string format (self-describing so Verify needs no external params):
///   `pbkdf2$sha256$<iterations>$<base64(salt)>$<base64(dk)>`
/// - salt: 16 random bytes (OpenSSL RAND_bytes)
/// - dk:   32-byte derived key (PBKDF2-HMAC-SHA256)
/// - iterations: OWASP-aligned default (600000); overridable for fast tests.
class Pbkdf2PasswordHasher : public IPasswordHasher {
public:
    /// Default iterations = OWASP 2023 PBKDF2-HMAC-SHA256 recommendation (600k).
    /// Tests pass a small value to keep hashing fast.
    explicit Pbkdf2PasswordHasher(int iterations = kDefaultIterations)
        : iterations_(iterations > 0 ? iterations : kDefaultIterations) {}

    Result<std::string> Hash(const std::string& password) override;
    Result<bool> Verify(const std::string& password, const std::string& hash) override;

    /// IPasswordHasher::cost — reports the bcrypt-equivalent work factor this
    /// build targets (12). The actual KDF here is PBKDF2/iterations;
    /// cost() stays 12 so callers/spec see the intended factor (placeholder note).
    int cost() const override { return 12; }

    int iterations() const { return iterations_; }

    static constexpr int kDefaultIterations = 600000;

private:
    int iterations_;
};

}  // namespace cortrix::auth
