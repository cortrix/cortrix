#include "cortrix/auth/api_key_auth.h"

#include <chrono>
#include <iomanip>
#include <sstream>

#include <openssl/evp.h>

#include "cortrix/auth/api_key_service.h"

namespace cortrix {

void ApiKeyAuth::LoadKeys(const std::vector<ApiKeyConfig>& keys) {
    std::lock_guard<std::mutex> lock(mu_);
    keys_.clear();
    for (const auto& k : keys) {
        keys_[k.key_hash] = k;
    }
}

Status ApiKeyAuth::Authenticate(const std::string& bearer_token, AuthContext* context) {
    if (bearer_token.empty()) {
        return Status::Unauthenticated("API key is empty");
    }

    std::string hash = HashKey(bearer_token);

    std::lock_guard<std::mutex> lock(mu_);
    auto it = keys_.find(hash);
    if (it == keys_.end()) {
        // [D3.5 r2 · P1] Fall back to the platform.db key store (bootstrap admin
        // key + /auth/api-keys-minted keys) when no static config key matches. A
        // valid DB key authenticates as admin (CE single-user model): platform.db
        // keys are all admin-minted; ValidateApiKey enforces active+not-expired and
        // bumps last_used_at. The mutex is held only over keys_; ValidateApiKey
        // touches its own DB handle, so calling it under the lock is safe (no
        // re-entrancy into ApiKeyAuth).
        if (db_keys_ != nullptr) {
            auto v = db_keys_->ValidateApiKey(bearer_token);
            if (v.ok()) {
                context->tenant_id = "default";  // CE single tenant (R1 fallback)
                context->user_id = v.value();    // owning user_id from api_keys row
                context->agent_id = "";
                context->namespaces.clear();     // empty = all-namespace access
                context->permissions = kPermRead | kPermWrite | kPermAdmin;
                return Status::Ok();
            }
        }
        return Status::Unauthenticated("Invalid API key");
    }

    const auto& kc = it->second;

    // Check expiration
    if (kc.expires_at > 0) {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now > kc.expires_at) {
            return Status::Unauthenticated("API key expired");
        }
    }

    // Fill AuthContext
    context->tenant_id = kc.tenant_id;
    context->user_id = kc.tenant_id;  // MVP: user_id == tenant_id
    context->agent_id = "";
    context->namespaces = kc.allowed_namespaces;
    context->permissions = kc.permissions;

    return Status::Ok();
}

Status ApiKeyAuth::Authorize(const AuthContext& ctx,
                              const std::string& ns,
                              int required_permission) {
    // Check permission bits
    if ((ctx.permissions & required_permission) != required_permission) {
        std::string perm_name;
        if (required_permission & kPermAdmin) perm_name = "ADMIN";
        else if (required_permission & kPermWrite) perm_name = "WRITE";
        else perm_name = "READ";
        return Status::PermissionDenied(perm_name + " permission required");
    }

    // Check namespace access
    if (!ns.empty() && !ctx.can_access_namespace(ns)) {
        return Status::PermissionDenied("Access denied for namespace '" + ns + "'");
    }

    return Status::Ok();
}

std::string ApiKeyAuth::HashKey(const std::string& plaintext) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, plaintext.data(), plaintext.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
}

}  // namespace cortrix
