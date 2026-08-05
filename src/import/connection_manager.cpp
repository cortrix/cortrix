#include "cortrix/import/connection_manager.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <chrono>
#include <cstdint>

#include "cortrix/id/ulid.h"
#include "cortrix/import/import_error.h"
#include "cortrix/observability/operation_log_emitter.h"

namespace cortrix::import {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Lower-hex encode (matches the codebase's hex idiom in api_key/auth).
std::string ToHex(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

bool FromHex(const std::string& hex, std::vector<unsigned char>& out) {
    if (hex.size() % 2 != 0) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return true;
}

constexpr int kGcmIvBytes = 12;
constexpr int kGcmTagBytes = 16;

}  // namespace

// --- InMemorySecretStore (AES-256-GCM, process-ephemeral key) ---

InMemorySecretStore::InMemorySecretStore() {
    // Process-ephemeral key — the plaintext DSN never sits unencrypted in the map.
    // A persistent KMS/Vault-backed store is the D3.5 / Phase-2 swap (L1 evolution).
    if (RAND_bytes(key_, sizeof(key_)) != 1) {
        // RAND failure is catastrophic for a credential store; zero the key so any
        // later Put/Get fails closed rather than encrypting under a predictable key.
        std::fill(std::begin(key_), std::end(key_), 0);
    }
}

std::string InMemorySecretStore::Put(const std::string& secret) {
    std::lock_guard<std::mutex> lock(mu_);

    unsigned char iv[kGcmIvBytes];
    if (RAND_bytes(iv, sizeof(iv)) != 1) return "";

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";

    std::string result;
    std::vector<unsigned char> ciphertext(secret.size());
    unsigned char tag[kGcmTagBytes];
    int len = 0;
    int ok = 1;
    ok &= EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok &= EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_, iv);
    if (ok == 1) {
        int clen = 0;
        ok &= EVP_EncryptUpdate(ctx, ciphertext.data(), &clen,
                                reinterpret_cast<const unsigned char*>(secret.data()),
                                static_cast<int>(secret.size()));
        len = clen;
        int flen = 0;
        ok &= EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &flen);
        len += flen;
        ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kGcmTagBytes, tag);
    }
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1) return "";

    // Blob layout: iv || tag || ciphertext, hex-encoded.
    std::string blob = ToHex(iv, kGcmIvBytes) + ToHex(tag, kGcmTagBytes) +
                       ToHex(ciphertext.data(), static_cast<size_t>(len));

    std::string key_id = "sk_" + std::to_string(next_id_++);
    blobs_[key_id] = blob;
    return key_id;
}

std::optional<std::string> InMemorySecretStore::Get(const std::string& key_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = blobs_.find(key_id);
    if (it == blobs_.end()) return std::nullopt;

    std::vector<unsigned char> blob;
    if (!FromHex(it->second, blob)) return std::nullopt;
    if (blob.size() < static_cast<size_t>(kGcmIvBytes + kGcmTagBytes)) return std::nullopt;

    const unsigned char* iv = blob.data();
    unsigned char* tag = blob.data() + kGcmIvBytes;
    const unsigned char* ct = blob.data() + kGcmIvBytes + kGcmTagBytes;
    int ct_len = static_cast<int>(blob.size() - kGcmIvBytes - kGcmTagBytes);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    std::vector<unsigned char> plain(static_cast<size_t>(ct_len) + 1);
    int len = 0;
    int ok = 1;
    ok &= EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok &= EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_, iv);
    int plen = 0;
    if (ok == 1) {
        ok &= EVP_DecryptUpdate(ctx, plain.data(), &plen, ct, ct_len);
        len = plen;
        ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kGcmTagBytes, tag);
    }
    int fin = 0;
    int verify = (ok == 1) ? EVP_DecryptFinal_ex(ctx, plain.data() + len, &fin) : 0;
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1 || verify <= 0) return std::nullopt;  // tag mismatch → tamper/decrypt fail

    len += fin;
    return std::string(reinterpret_cast<char*>(plain.data()), static_cast<size_t>(len));
}

void InMemorySecretStore::Erase(const std::string& key_id) {
    std::lock_guard<std::mutex> lock(mu_);
    blobs_.erase(key_id);
}

// --- InMemoryConnectionStore ---

Status InMemoryConnectionStore::Insert(const Record& rec) {
    std::lock_guard<std::mutex> lock(mu_);
    if (records_.count(rec.ref_id)) {
        return Status::AlreadyExists("db_connection ref already exists: " + rec.ref_id);
    }
    records_[rec.ref_id] = rec;
    return Status::Ok();
}

std::optional<IConnectionStore::Record> InMemoryConnectionStore::Find(
    const ConnectionRefId& ref_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = records_.find(ref_id);
    if (it == records_.end()) return std::nullopt;
    return it->second;
}

std::vector<IConnectionStore::Record> InMemoryConnectionStore::ListByTenant(
    const TenantId& tenant_id) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Record> out;
    for (const auto& [ref, rec] : records_) {
        if (rec.tenant_id == tenant_id) out.push_back(rec);
    }
    return out;
}

Status InMemoryConnectionStore::MarkRevoked(const ConnectionRefId& ref_id,
                                            int64_t revoked_at_ms,
                                            const std::string& revoked_by,
                                            const std::string& reason) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = records_.find(ref_id);
    if (it == records_.end()) return Status::NotFound("db_connection ref not found: " + ref_id);
    it->second.revoked_at_ms = revoked_at_ms;
    it->second.revoked_by = revoked_by;
    it->second.revoke_reason = reason;
    return Status::Ok();
}

// --- ConnectionManager ---

ConnectionManager::ConnectionManager(std::shared_ptr<ISecretStore> secret_store,
                                     std::shared_ptr<IConnectionStore> conn_store,
                                     std::shared_ptr<observability::IOperationLogger> op_logger,
                                     ConnectionManagerConfig config)
    : secret_store_(std::move(secret_store)),
      conn_store_(std::move(conn_store)),
      op_logger_(std::move(op_logger)),
      config_(config) {}

namespace {

// Parse host + db_name out of a postgres DSN for the non-sensitive metadata columns
// — WITHOUT keeping the password. Accepts "postgres://user:pass@host:port/db?..."
// and "host=... dbname=..." keyword form; best-effort (metadata only, never used to
// connect). The password is never extracted / stored here (R2).
void ParseDsnMetadata(const std::string& dsn, std::string& host, std::string& db_name) {
    host.clear();
    db_name.clear();
    auto scheme = dsn.find("://");
    if (scheme != std::string::npos) {
        std::string rest = dsn.substr(scheme + 3);
        auto at = rest.find('@');
        std::string after_auth = (at != std::string::npos) ? rest.substr(at + 1) : rest;
        auto slash = after_auth.find('/');
        std::string hostport = (slash != std::string::npos) ? after_auth.substr(0, slash)
                                                             : after_auth;
        auto colon = hostport.find(':');
        host = (colon != std::string::npos) ? hostport.substr(0, colon) : hostport;
        if (slash != std::string::npos) {
            std::string db = after_auth.substr(slash + 1);
            auto q = db.find('?');
            db_name = (q != std::string::npos) ? db.substr(0, q) : db;
        }
        return;
    }
    // keyword form: scan "host=" / "dbname=".
    auto extract = [&](const std::string& key) -> std::string {
        auto p = dsn.find(key);
        if (p == std::string::npos) return "";
        p += key.size();
        auto end = dsn.find_first_of(" \t", p);
        return dsn.substr(p, end == std::string::npos ? std::string::npos : end - p);
    };
    host = extract("host=");
    db_name = extract("dbname=");
}

}  // namespace

Result<ConnectionRefId> ConnectionManager::Register(const std::string& name,
                                                    const std::string& dsn,
                                                    const TenantId& tenant_id,
                                                    const std::string& registered_by,
                                                    std::optional<int> expire_days) {
    if (name.empty() || dsn.empty() || tenant_id.empty()) {
        return ImportStatus(ImportErrorCode::kInvalidSql,
                          "name, dsn and tenant_id are required to register a connection");
    }
    int days = expire_days.value_or(config_.expire_default_days);
    if (days <= 0 || days > config_.expire_max_days) {
        return ImportStatus(ImportErrorCode::kInvalidSql,
                          "expire_days out of range (1.." +
                              std::to_string(config_.expire_max_days) + ")");
    }

    std::string secret_key_id = secret_store_->Put(dsn);
    if (secret_key_id.empty()) {
        return ImportStatus(ImportErrorCode::kConnectionFailed,
                          "secret store failed to encrypt the connection credential");
    }

    IConnectionStore::Record rec;
    rec.ref_id = "db_conn_" + cortrix::id::GenerateUlid();
    rec.name = name;
    rec.tenant_id = tenant_id;
    rec.secret_key_id = secret_key_id;
    ParseDsnMetadata(dsn, rec.host, rec.db_name);
    rec.registered_by = registered_by;
    rec.registered_at_ms = NowMs();
    rec.expires_at_ms = rec.registered_at_ms + static_cast<int64_t>(days) * 24 * 3600 * 1000;

    Status ins = conn_store_->Insert(rec);
    if (!ins.ok()) {
        secret_store_->Erase(secret_key_id);  // don't leak an orphan secret
        return ins;
    }

    // S6 (F16a-rev-6): audit the register. resource_id = ref_id (NEVER the DSN, R2).
    if (op_logger_) {
        auto entry = observability::MakeEngineEntry(
            observability::EmitSite::kSpcPipeline, "db_connection_register",
            /*namespace_id=*/std::nullopt, /*resource_id=*/rec.ref_id,
            "register DB connection " + name);
        entry.user_id = registered_by.empty() ? "anonymous" : registered_by;
        op_logger_->Log(entry);
    }
    return rec.ref_id;
}

Result<std::string> ConnectionManager::ResolveDsn(const ConnectionRefId& ref_id,
                                                  const AuthContext& auth_ctx) {
    auto rec = conn_store_->Find(ref_id);
    if (!rec) {
        return ImportStatus(ImportErrorCode::kAuthDenied,
                          "connection ref not found: " + ref_id);
    }
    // D7 tenant boundary: a ref may only be resolved by its owning tenant. We do NOT
    // distinguish "not found" from "wrong tenant" in the public message (anti-enumeration), but
    // a cross-tenant attempt is its own code so the Agent can re-pick a valid ref.
    if (rec->tenant_id != auth_ctx.tenant_id) {
        throw ImportException(ImportErrorCode::kCrossTenantRef,
                            {{"connection_ref", ref_id}, {"tenant_id", auth_ctx.tenant_id}},
                            "connection ref belongs to another tenant");
    }
    if (rec->revoked_at_ms.has_value()) {
        return ImportStatus(ImportErrorCode::kAuthDenied, "connection ref has been revoked: " + ref_id);
    }
    if (rec->expires_at_ms <= NowMs()) {
        return ImportStatus(ImportErrorCode::kAuthDenied,
                          "connection ref expired; re-register: " + ref_id);
    }

    auto dsn = secret_store_->Get(rec->secret_key_id);
    if (!dsn) {
        return ImportStatus(ImportErrorCode::kConnectionFailed,
                          "secret store could not resolve the credential");
    }
    return *dsn;
}

std::vector<ConnectionInfo> ConnectionManager::ListConnections(const TenantId& tenant_id) {
    std::vector<ConnectionInfo> out;
    for (const auto& rec : conn_store_->ListByTenant(tenant_id)) {
        ConnectionInfo info;
        info.ref_id = rec.ref_id;
        info.name = rec.name;
        info.tenant_id = rec.tenant_id;
        info.host = rec.host;
        info.db_name = rec.db_name;
        info.registered_by = rec.registered_by;
        info.registered_at_ms = rec.registered_at_ms;
        info.expires_at_ms = rec.expires_at_ms;
        info.revoked = rec.revoked_at_ms.has_value();
        out.push_back(info);  // NOTE: never carries the DSN
    }
    return out;
}

Status ConnectionManager::Revoke(const ConnectionRefId& ref_id,
                                 const AuthContext& auth_ctx,
                                 const std::string& reason) {
    auto rec = conn_store_->Find(ref_id);
    if (!rec) return ImportStatus(ImportErrorCode::kAuthDenied, "connection ref not found: " + ref_id);
    if (rec->tenant_id != auth_ctx.tenant_id) {
        return ImportStatus(ImportErrorCode::kCrossTenantRef,
                          "connection ref belongs to another tenant: " + ref_id);
    }
    Status s = conn_store_->MarkRevoked(ref_id, NowMs(), auth_ctx.user_id, reason);
    if (!s.ok()) return s;
    secret_store_->Erase(rec->secret_key_id);  // drop the secret on revoke

    // S6 (F16a-rev-6): audit the revoke.
    if (op_logger_) {
        auto entry = observability::MakeEngineEntry(
            observability::EmitSite::kSpcPipeline, "db_connection_revoke",
            /*namespace_id=*/std::nullopt, /*resource_id=*/ref_id,
            "revoke DB connection: " + reason);
        entry.user_id = auth_ctx.user_id.empty() ? "anonymous" : auth_ctx.user_id;
        op_logger_->Log(entry);
    }
    return Status::Ok();
}

}  // namespace cortrix::import
