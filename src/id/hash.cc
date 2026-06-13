#include "cortrix/id/hash.h"

#include <openssl/rand.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <string>

#include "cortrix/util/siphash.h"

namespace cortrix::id {

// The process-global deployment key (zero until LoadOrBootstrapHashKey runs).
SipHashKey kDeploymentHashKey;

namespace {

constexpr int kKeyBytes = 16;  // 128-bit SipHash key (k0‖k1, little-endian)

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 16 random bytes → 32-char hex, used as the opaque auth_secrets.id (same idiom as
// JwtSecretService::RandomHexId — auth_secrets.id is just a unique key).
std::string RandomHexId() {
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

// Assemble a SipHashKey from 16 raw bytes (little-endian: b[0..7]=k0, b[8..15]=k1).
// Little-endian matches util::SipHash24's word load, so the on-disk key bytes and
// the hash are byte-order independent.
SipHashKey KeyFromBytes(const unsigned char* b) {
    SipHashKey k;
    for (int i = 0; i < 8; ++i) k.k0 |= static_cast<uint64_t>(b[i]) << (8 * i);
    for (int i = 0; i < 8; ++i) k.k1 |= static_cast<uint64_t>(b[8 + i]) << (8 * i);
    return k;
}

// Parse exactly 32 hex chars into 16 bytes; false if wrong length or non-hex.
bool ParseHexKey(const char* hex, unsigned char out[kKeyBytes]) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 2 * kKeyBytes; ++i) {
        if (hex[i] == '\0') return false;  // too short
    }
    if (hex[2 * kKeyBytes] != '\0') return false;  // too long
    for (int i = 0; i < kKeyBytes; ++i) {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

// Persist a 16-byte key as the single status='current' siphash_id_key row.
Status InsertCurrentKey(sqlite3* db, const std::string& id,
                        const unsigned char key[kKeyBytes]) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO auth_secrets(id, secret_type, value, status, created_at, expires_at) "
        "VALUES(?, 'siphash_id_key', ?, 'current', ?, NULL)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Status::Internal(std::string("siphash key insert prepare failed: ") +
                                sqlite3_errmsg(db));
    }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, key, kKeyBytes, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, NowMs());
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return Status::Internal(std::string("siphash key insert step failed: ") +
                                sqlite3_errmsg(db));
    }
    return Status::Ok();
}

}  // namespace

Status LoadOrBootstrapHashKey(sqlite3* db) {
    if (db == nullptr) {
        return Status::InvalidArgument("platform.db handle is null");
    }

    // (1) env override — write through so a restart without the env still loads the
    //     same key from platform.db (mirrors JwtSecretService env path).
    if (const char* env = std::getenv("CORTRIX_SIPHASH_ID_KEY");
        env != nullptr && env[0] != '\0') {
        unsigned char raw[kKeyBytes];
        if (!ParseHexKey(env, raw)) {
            return Status::InvalidArgument(
                "CORTRIX_SIPHASH_ID_KEY must be 32 hex chars (16 bytes)");
        }
        if (sqlite3_exec(db,
                         "UPDATE auth_secrets SET status='retired' "
                         "WHERE secret_type='siphash_id_key' AND status='current'",
                         nullptr, nullptr, nullptr) != SQLITE_OK) {
            return Status::Internal(std::string("siphash key env-override demote failed: ") +
                                    sqlite3_errmsg(db));
        }
        Status st = InsertCurrentKey(db, RandomHexId(), raw);
        if (!st.ok()) return st;
        kDeploymentHashKey = KeyFromBytes(raw);
        return Status::Ok();
    }

    // (2) load existing status='current' siphash_id_key if present.
    sqlite3_stmt* stmt = nullptr;
    const char* sel =
        "SELECT value FROM auth_secrets "
        "WHERE secret_type='siphash_id_key' AND status='current' LIMIT 1";
    if (sqlite3_prepare_v2(db, sel, -1, &stmt, nullptr) != SQLITE_OK) {
        return Status::Internal(std::string("siphash key select prepare failed: ") +
                                sqlite3_errmsg(db));
    }
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        const int len = sqlite3_column_bytes(stmt, 0);
        if (blob != nullptr && len == kKeyBytes) {
            kDeploymentHashKey = KeyFromBytes(static_cast<const unsigned char*>(blob));
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    if (found) return Status::Ok();

    // (3) none → generate 16 random bytes + persist as current.
    unsigned char raw[kKeyBytes];
    if (RAND_bytes(raw, kKeyBytes) != 1) {
        return Status::Internal("siphash key RAND_bytes failed");
    }
    Status st = InsertCurrentKey(db, RandomHexId(), raw);
    if (!st.ok()) return st;
    kDeploymentHashKey = KeyFromBytes(raw);
    return Status::Ok();
}

BlockId HashChildIdToBlockId(const ChildId& id) {
    return util::SipHash24(id.data(), id.size(), kDeploymentHashKey.k0,
                           kDeploymentHashKey.k1);
}

void SetDeploymentHashKeyForTesting(SipHashKey key) { kDeploymentHashKey = key; }

}  // namespace cortrix::id
