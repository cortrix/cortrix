#include "cortrix/auth/password_utils.h"

#include <openssl/rand.h>

#include <cctype>
#include <cstddef>
#include <string>

namespace cortrix::auth {

namespace {
constexpr int kPasswordMaxLength = 128;  // bcrypt DoS guard
}  // namespace

bool ValidatePassword(const std::string& password, int min_length) {
    const int len = static_cast<int>(password.size());
    if (len < min_length || len > kPasswordMaxLength) return false;
    bool has_letter = false;
    bool has_digit = false;
    for (unsigned char c : password) {
        if (std::isalpha(c)) has_letter = true;
        else if (std::isdigit(c)) has_digit = true;
    }
    return has_letter && has_digit;
}

bool ValidateEmail(const std::string& email) {
    if (email.empty() || email.size() > 254) return false;  // RFC max length
    const std::size_t at = email.find('@');
    if (at == std::string::npos) return false;
    if (email.find('@', at + 1) != std::string::npos) return false;  // exactly one '@'

    const std::string local = email.substr(0, at);
    const std::string domain = email.substr(at + 1);
    if (local.empty() || domain.empty()) return false;

    // Domain must contain a dot, not at either end, and no consecutive dots.
    const std::size_t dot = domain.find('.');
    if (dot == std::string::npos) return false;
    if (domain.front() == '.' || domain.back() == '.') return false;
    if (domain.find("..") != std::string::npos) return false;

    // No whitespace anywhere.
    for (unsigned char c : email) {
        if (std::isspace(c)) return false;
    }
    return true;
}

std::string GenerateUserId() {
    unsigned char buf[4];
    RAND_bytes(buf, sizeof(buf));
    static const char* kHex = "0123456789abcdef";
    std::string out = "usr_";
    out.reserve(12);
    for (unsigned char b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

}  // namespace cortrix::auth
