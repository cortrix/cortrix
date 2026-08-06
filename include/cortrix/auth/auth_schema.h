#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/status.h"

namespace cortrix::auth {

/// Current Auth schema version. Bumped only for Phase 1 internal
/// schema evolution; this build creates the v1 set from scratch.
constexpr int kAuthSchemaVersion = 1;

/// The platform.db DDL emitted by the Auth SchemaProvider: the 7 Auth
/// tables (total tables = 7):
///   v0 4 tables: users / refresh_tokens / verification_codes / token_blacklist
///   v1.0 adds 3 tables: auth_secrets (topic 1.1C) / auth_config (topic 8) / api_keys (topic 7D)
/// faithfully transcribed from auth-auth-system.md–
///
/// 🔒 DB SEPARATION (important — do NOT confuse with the catalog `users` table):
/// these tables live in **platform.db**, a *different*
/// SQLite file from catalog.db. catalog.db already has a thin `users`
/// (user_id/email/created_at, relationship model, src/catalog/catalog_schema.cpp)
/// — that is the FROZEN L0 contract and is left untouched. The richer auth
/// `users` (password_hash / status / login_attempts / …) is a platform.db table.
/// Same table *name*, different *database file*, different grain — no collision.
/// Reconciling the two (one users SoT) is cross-Feature wiring = integration, not integration.
///
/// SQLite-dialect notes (platform.db is SQLite WAL): types are stored
/// per affinity; BOOLEAN/bit flags as INTEGER 0/1; `value BLOB` on auth_secrets
/// holds the raw 64-byte HS256 secret. IF NOT EXISTS on every object so a re-run
/// on an already-migrated db is a no-op (defensive; the migrator's schema_version
/// gate is the primary idempotency mechanism).
extern const char* const kAuthSchemaSql;

/// The Auth ISchemaProvider: owns the 7 Auth tables in platform.db. Registered
/// with a SchemaMigrator that targets platform.db (NOT the catalog migrator
/// chain — different database). Reuses the frozen L0 ISchemaProvider/SchemaMigrator
/// scaffolding (catalog/schema_provider.h) verbatim — no new migration engine.
class AuthSchemaProvider : public catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "auth"; }
    int CurrentVersion() const override { return kAuthSchemaVersion; }
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::auth
