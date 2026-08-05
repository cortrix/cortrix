#pragma once
#include <string>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::auth {

// ============================ Validation ==========================
// These are bcrypt-independent and fully testable on their own (S2 test cases
// Validate_* + Validate_Email*).

/// Password complexity: length in [min_length, 128] AND contains at
/// least one letter and one digit. The regex equivalent is
/// ^(?=.*[a-zA-Z])(?=.*\d).{min,128}$. `min_length` comes from
/// AuthConfig.password_min_length (default 8, topic 2).
bool ValidatePassword(const std::string& password, int min_length = 8);

/// Basic RFC-5322-ish email format check (P08 §4.1 step 1): exactly one '@',
/// non-empty local part, a domain with at least one '.', no spaces. Deliberately
/// permissive (delivery is the real validator) but rejects obvious garbage.
bool ValidateEmail(const std::string& email);

/// Generate a user id: "usr_" + 8 lowercase hex chars (P08 §2.10 / §4.1 step 3).
/// Uses OpenSSL RAND_bytes (4 random bytes → 8 hex).
std::string GenerateUserId();

// ======================= Password hashing abstraction =======================

/// Pluggable password hasher (P08 §4.2 — bcrypt cost=12). The interface keeps the
/// AuthService / Register flow independent of the concrete bcrypt library choice
/// (a build/license decision); BcryptPasswordHasher is the production impl, and
/// tests can substitute a fast fake. All methods are pure (no DB / IO).
class IPasswordHasher {
public:
    virtual ~IPasswordHasher() = default;

    /// Hash `password`, returning the encoded hash string (bcrypt: `$2b$<cost>$...`).
    /// Error → CX_ERR_AUTH_BCRYPT_TIMEOUT / CX_ERR_INTERNAL_ERROR.
    virtual Result<std::string> Hash(const std::string& password) = 0;

    /// Constant-time-ish verify of `password` against a stored `hash`. Returns
    /// false (not an error) on mismatch; an error Status only for a malformed
    /// hash / internal failure.
    virtual Result<bool> Verify(const std::string& password,
                                const std::string& hash) = 0;

    /// The cost factor this hasher applies (bcrypt work factor; 12 per §4.2).
    virtual int cost() const = 0;
};

}  // namespace cortrix::auth
