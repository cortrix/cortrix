#include "cortrix/logging/sanitizer.h"

#include <fstream>
#include <memory>
#include <sstream>

#include <openssl/evp.h>
#include "yaml-cpp/yaml.h"

#include "cortrix/logging/logging.h"

namespace cortrix::logging {

namespace {

/// SHA-256(text) -> 64-char lowercase hex. Same OpenSSL EVP the project uses
/// elsewhere (spc/data_cleaner.cpp, auth/api_key_auth.cpp). Used here for
/// non-reversible correlation hashing of doc_id / user_id / content.
std::string Sha256Hex(const std::string& text) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, text.data(), text.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

void LoadSet(const YAML::Node& root, const char* key,
             std::unordered_set<std::string>& out) {
    if (!root[key] || !root[key].IsSequence()) return;
    for (const auto& item : root[key]) {
        out.insert(item.as<std::string>());
    }
}

// ---- per-rule transforms (design sec 6.1) ----

/// PII mask: email -> ****@***, IPv4 -> keep first two octets (192.168.***.***),
/// everything else -> ****.
std::string MaskPii(const std::string& field, const std::string& value) {
    if (field == "email") {
        return "****@***";
    }
    if (field == "ip_address") {
        // Keep the first two octets, mask the host portion.
        size_t first = value.find('.');
        size_t second = (first == std::string::npos)
                            ? std::string::npos
                            : value.find('.', first + 1);
        if (second != std::string::npos) {
            return value.substr(0, second) + ".***.***";
        }
        return "***";  // not a dotted IPv4 (e.g. IPv6) -> fully mask
    }
    // phone / real_name / postal_code and any other PII field.
    return "****";
}

/// Partial mask for api_key_prefix: keep a debuggable head + tail (sk-prod-***xyz).
std::string MaskPartial(const std::string& value) {
    constexpr size_t kHead = 8;
    constexpr size_t kTail = 3;
    if (value.size() <= kHead + kTail) {
        return "***";  // too short to reveal anything safely
    }
    return value.substr(0, kHead) + "***" + value.substr(value.size() - kTail);
}

/// Content truncation: query_text keeps a 50-char preview + full hash; the
/// remaining content fields are hash-only (no preview).
std::string TruncateContent(const std::string& field, const std::string& value) {
    if (field == "query_text") {
        constexpr size_t kPreview = 50;
        std::string preview =
            value.size() > kPreview ? value.substr(0, kPreview) : value;
        return preview + "...[sha256:" + Sha256Hex(value) + "]";
    }
    // document_content / memory_text / prompt -> hash only.
    return "[sha256:" + Sha256Hex(value) + "]";
}

}  // namespace

LogSanitizer::LogSanitizer(const std::string& yaml_text) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const std::exception& e) {
        CORTRIX_LOG_WARN("security",
                         "LogSanitizer: failed to parse field rules ({}), "
                         "no fields will be sanitized", e.what());
        return;
    }
    LoadSet(root, "P0_redact_completely", p0_redact_);
    LoadSet(root, "P0_mask_partial", p0_mask_);
    LoadSet(root, "P1_redact_pii", p1_pii_);
    LoadSet(root, "P1_truncate_content", p1_truncate_);
    LoadSet(root, "P2_hash_only", p2_hash_);
}

bool LogSanitizer::IsControlled(const std::string& field_name) const {
    return p0_redact_.count(field_name) || p0_mask_.count(field_name) ||
           p1_pii_.count(field_name) || p1_truncate_.count(field_name) ||
           p2_hash_.count(field_name);
}

std::string LogSanitizer::SanitizeField(const std::string& field_name,
                                       const std::string& value) const {
    if (p0_redact_.count(field_name))   return "****";
    if (p0_mask_.count(field_name))     return MaskPartial(value);
    if (p1_pii_.count(field_name))      return MaskPii(field_name, value);
    if (p1_truncate_.count(field_name)) return TruncateContent(field_name, value);
    if (p2_hash_.count(field_name))     return "[sha256:" + Sha256Hex(value) + "]";
    return value;  // not controlled
}

nlohmann::json LogSanitizer::SanitizeValue(const std::string& key,
                                           const nlohmann::json& value) const {
    // Objects / arrays: recurse so nested sensitive fields are caught too.
    if (value.is_object() || value.is_array()) {
        return Sanitize(value);
    }
    if (!IsControlled(key)) {
        return value;  // pass through untouched
    }
    // Controlled scalar: stringify (numbers/bools coerced) then apply the rule.
    std::string str = value.is_string() ? value.get<std::string>() : value.dump();
    return SanitizeField(key, str);
}

nlohmann::json LogSanitizer::Sanitize(const nlohmann::json& payload) const {
    if (payload.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            out[it.key()] = SanitizeValue(it.key(), it.value());
        }
        return out;
    }
    if (payload.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& elem : payload) {
            // Array elements have no key of their own; recurse for nested
            // objects, otherwise pass the scalar through.
            out.push_back(elem.is_object() || elem.is_array() ? Sanitize(elem)
                                                              : elem);
        }
        return out;
    }
    return payload;  // top-level scalar: no key to match against
}

// ---- process-wide instance ----

namespace {
std::unique_ptr<LogSanitizer> g_sanitizer;
}  // namespace

void LogSanitizer::Initialize(const std::string& config_path) {
    std::ifstream f(config_path);
    if (!f.good()) {
        CORTRIX_LOG_WARN("security",
                         "LogSanitizer: sensitive_fields.yaml not found at '{}', "
                         "sanitization disabled", config_path);
        g_sanitizer = std::make_unique<LogSanitizer>("");  // empty pass-through
        return;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    g_sanitizer = std::make_unique<LogSanitizer>(buf.str());
    CORTRIX_LOG_INFO("security", "LogSanitizer initialized from {}", config_path);
}

const LogSanitizer* LogSanitizer::Global() {
    return g_sanitizer.get();
}

}  // namespace cortrix::logging
