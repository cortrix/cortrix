#include "cortrix/common/agent_llm_config_codec.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "cortrix/common/i_global_config.h"

namespace cortrix {
namespace agent_llm_codec {

namespace {

constexpr int kIvBytes = 12;
constexpr int kTagBytes = 16;

// Process-wide AES-256 key for agent_llm.api_key at-rest encryption. Derived from
// CORTRIX_CONFIG_SECRET (SHA-256) when set so the ciphertext survives a restart
// (FileBased / GUC persistence); otherwise a process-ephemeral random key (the
// api_key is then valid only for this process lifetime — acceptable for dev /
// InMemory). The key never leaves this TU.
const std::array<unsigned char, 32>& EncryptionKey() {
    static const std::array<unsigned char, 32> key = [] {
        std::array<unsigned char, 32> k{};
        if (const char* secret = std::getenv("CORTRIX_CONFIG_SECRET");
            secret != nullptr && secret[0] != '\0') {
            SHA256(reinterpret_cast<const unsigned char*>(secret),
                   std::strlen(secret), k.data());
        } else {
            RAND_bytes(k.data(), static_cast<int>(k.size()));
        }
        return k;
    }();
    return key;
}

std::string ToHex(const unsigned char* data, size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

bool FromHex(const std::string& hex, std::string* out) {
    if (hex.size() % 2 != 0) return false;
    out->clear();
    out->reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

// Encrypt plaintext → hex(iv || tag || ciphertext). "" → "" (no blob stored).
std::string Encrypt(const std::string& plaintext) {
    if (plaintext.empty()) return "";
    const auto& key = EncryptionKey();
    unsigned char iv[kIvBytes];
    if (RAND_bytes(iv, kIvBytes) != 1) return "";

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    std::string cipher(plaintext.size(), '\0');
    unsigned char tag[kTagBytes];
    int len = 0, clen = 0, flen = 0;
    bool ok = true;
    ok &= EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok &= EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv) == 1;
    if (ok) {
        ok &= EVP_EncryptUpdate(
                  ctx, reinterpret_cast<unsigned char*>(cipher.data()), &len,
                  reinterpret_cast<const unsigned char*>(plaintext.data()),
                  static_cast<int>(plaintext.size())) == 1;
        clen = len;
        ok &= EVP_EncryptFinal_ex(
                  ctx, reinterpret_cast<unsigned char*>(cipher.data()) + clen, &flen) == 1;
        clen += flen;
        ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes, tag) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return "";
    return ToHex(iv, kIvBytes) + ToHex(tag, kTagBytes) +
           ToHex(reinterpret_cast<const unsigned char*>(cipher.data()), clen);
}

// Decrypt hex(iv || tag || ciphertext) → plaintext. Any failure → "" (fail-soft;
// a key change / corrupt blob just drops the api_key rather than throwing).
std::string Decrypt(const std::string& blob_hex) {
    if (blob_hex.empty()) return "";
    std::string raw;
    if (!FromHex(blob_hex, &raw)) return "";
    if (raw.size() < static_cast<size_t>(kIvBytes + kTagBytes)) return "";
    const auto& key = EncryptionKey();
    const unsigned char* iv = reinterpret_cast<const unsigned char*>(raw.data());
    const unsigned char* tag = iv + kIvBytes;
    const unsigned char* ct = tag + kTagBytes;
    int ct_len = static_cast<int>(raw.size() - kIvBytes - kTagBytes);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    std::string plain(ct_len > 0 ? ct_len : 0, '\0');
    int len = 0, plen = 0;
    bool ok = true;
    ok &= EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok &= EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv) == 1;
    if (ok && ct_len > 0) {
        ok &= EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()),
                                &len, ct, ct_len) == 1;
        plen = len;
    }
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes,
                              const_cast<unsigned char*>(tag)) == 1;
    int verify = (ok ? EVP_DecryptFinal_ex(
                           ctx, reinterpret_cast<unsigned char*>(plain.data()) + plen, &len)
                     : 0);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || verify != 1) return "";
    plain.resize(plen + len);
    return plain;
}

std::string GetOr(const IGlobalConfig& cfg, const std::string& key,
                  const std::string& fallback) {
    auto r = cfg.GetString(key);
    return r.ok() ? r.value() : fallback;
}

}  // namespace

AgentLlmConfig Load(const IGlobalConfig& cfg) {
    AgentLlmConfig out;  // struct defaults
    out.provider = GetOr(cfg, "agent_llm.provider", out.provider);
    out.model = GetOr(cfg, "agent_llm.model", out.model);
    out.base_url = GetOr(cfg, "agent_llm.base_url", out.base_url);
    out.api_key = Decrypt(GetOr(cfg, "agent_llm.api_key_enc", ""));
    if (auto r = cfg.GetInt("agent_llm.max_tokens"); r.ok()) out.max_tokens = r.value();
    if (auto r = cfg.GetFloat("agent_llm.temperature"); r.ok())
        out.temperature = static_cast<double>(r.value());
    return out;
}

void Store(const AgentLlmConfig& in,
           const std::function<void(const std::string&, const std::string&)>& set) {
    set("agent_llm.provider", in.provider);
    set("agent_llm.model", in.model);
    set("agent_llm.base_url", in.base_url);
    set("agent_llm.api_key_enc", Encrypt(in.api_key));
    set("agent_llm.max_tokens", std::to_string(in.max_tokens));
    set("agent_llm.temperature", std::to_string(in.temperature));
}

std::string MaskApiKey(const std::string& api_key) {
    if (api_key.empty()) return "";
    if (api_key.size() <= 4) return "...";
    return api_key.substr(0, 4) + "...";
}

}  // namespace agent_llm_codec

// ----- IGlobalConfig default impls (reverse hook) ----------------------------
// The default GetAgentLlmConfig reads via the generic GetString/GetInt/GetFloat
// (available on the interface). The default SetAgentLlmConfig is a no-op: the
// interface has no generic setter, so a writable backend (InMemory / FileBased)
// overrides it to route Store() through its own Set(key,value). A read-only
// backend keeps the no-op (config is immutable there).
AgentLlmConfig IGlobalConfig::GetAgentLlmConfig() const {
    return agent_llm_codec::Load(*this);
}

void IGlobalConfig::SetAgentLlmConfig(const AgentLlmConfig& /*cfg*/) {
    // no-op default — see note above.
}

}  // namespace cortrix
