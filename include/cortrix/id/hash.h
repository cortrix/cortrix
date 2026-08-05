#pragma once
#include <cstdint>

#include "cortrix/common/status.h"
#include "cortrix/id/types.h"

// Forward-declare the SQLite handle; full <sqlite3.h> is pulled in by the .cpp.
typedef struct sqlite3 sqlite3;

// Cortrix global ID hashing (ARCH §1.8.2). A Block's uint64 P-HNSW label is the
// SipHash-2-4 of its business ULID under a per-deployment 128-bit key:
//   block_id = HashChildIdToBlockId(child_id) = SipHash24(child_id, k0, k1)
// The key is loaded once at startup from platform.db (auth_secrets, secret_type=
// 'siphash_id_key'), mirroring JwtSecretService::LoadOrInit. It must stay
// stable for the life of an index (so the persisted P-HNSW labels keep pointing at
// the right nodes), hence it is generated once and persisted — per deployment, not
// per namespace (§1.8.2 V4 ruling 3).
namespace cortrix::id {

/// The 128-bit SipHash key, as two 64-bit halves (the persisted deployment key).
struct SipHashKey {
    uint64_t k0 = 0;
    uint64_t k1 = 0;
};

/// Process-global deployment key. Zero until LoadOrBootstrapHashKey() runs at
/// startup; HashChildIdToBlockId() reads it. Set once at boot, read-only afterwards
/// (startup is single-threaded, before any worker starts — no locking needed).
extern SipHashKey kDeploymentHashKey;

/// Startup key load, idempotent (mirrors JwtSecretService::LoadOrInit):
///   1) if env `CORTRIX_SIPHASH_ID_KEY` (32 hex chars = 16 bytes, k0‖k1 LE) is set →
///      use it and upsert it as the current siphash_id_key;
///   2) else if a status='current' siphash_id_key row exists → load it;
///   3) else generate 16 random bytes → persist as current → use it.
/// On success kDeploymentHashKey is set. `platform_db` is an open handle whose
/// auth_secrets table already exists (PlatformDb::Open migrated it). A platform.db
/// write failure is FATAL at startup (caller refuses to start), same as JWT init.
///
/// NOTE: wiring this into the main.cpp boot sequence (platform.db open → before
/// feature init) is integration work — JwtSecretService::LoadOrInit is
/// likewise not yet wired into main.cpp; both land together. This function +
/// HashChildIdToBlockId are the standalone, unit-tested deliverable.
Status LoadOrBootstrapHashKey(sqlite3* platform_db);

/// Map a business ULID (child_id / doc_id / memory_id / …) to its uint64 Block id
/// under kDeploymentHashKey. Hashes id.size() bytes — length is part of the input.
BlockId HashChildIdToBlockId(const ChildId& id);

/// Test seam: set the process-global key directly (bypasses platform.db), so unit
/// tests can exercise HashChildIdToBlockId deterministically without a db.
void SetDeploymentHashKeyForTesting(SipHashKey key);

}  // namespace cortrix::id
