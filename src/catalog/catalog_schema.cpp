#include "cortrix/catalog/catalog_schema.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::catalog {

// catalog.db framework schema — transcribed from F12-two-layer-mapping.md §4.1
// (mirrors ARCHITECTURE.md §2.4). Kept as one DDL batch so the migrator applies
// it atomically. See catalog_schema.h for the SQLite-dialect notes (JSONB/BIGINT
// type names kept; '{}'::jsonb casts → plain '{}' default; BOOLEAN → INTEGER).
//
// IF NOT EXISTS on every object so a re-run on an already-migrated db is a no-op
// even outside the migrator's version gate (defensive; the migrator's
// schema_version gate is the primary idempotency mechanism).
const char* const kCatalogSchemaSql = R"SQL(
-- ===== 6 main tables =====

-- 1. file_locations (OPEN-1 dedup core + OPEN-2 three-stage GC + D15 Blob URI).
--    Full 16 columns, aligned with ARCH §2.4.
CREATE TABLE IF NOT EXISTS file_locations (
    file_hash       TEXT PRIMARY KEY,
    unit_id         TEXT NOT NULL,
    ns_id           TEXT NOT NULL,
    tenant_id       TEXT NOT NULL,
    size_bytes      INTEGER NOT NULL DEFAULT 0,
    chunk_count     INTEGER NOT NULL DEFAULT 0,
    blob_uri        TEXT,
    storage_type    TEXT,
    ref_count       INTEGER NOT NULL DEFAULT 0,
    isolated        INTEGER NOT NULL DEFAULT 0,
    first_seen_at   INTEGER NOT NULL,
    last_accessed   INTEGER,
    status          TEXT NOT NULL DEFAULT 'active',
    deleted_at      INTEGER,
    created_at      INTEGER NOT NULL,
    FOREIGN KEY (unit_id) REFERENCES units(unit_id)
);

-- 2. ns_units (NS→Unit mapping; Phase 1 1:1, Phase 2 1:N).
CREATE TABLE IF NOT EXISTS ns_units (
    ns_id        TEXT NOT NULL,
    unit_id      TEXT NOT NULL,
    is_primary   INTEGER NOT NULL DEFAULT 1,
    created_at   INTEGER NOT NULL,
    PRIMARY KEY (ns_id, unit_id),
    FOREIGN KEY (ns_id) REFERENCES namespaces(ns_id),
    FOREIGN KEY (unit_id) REFERENCES units(unit_id)
);

-- 3. units (Unit metadata + the 9 reserved Phase 2 fields:
--    state / index_type / sealed_at / archived_at / vector_count / size_bytes /
--    last_write_at / seal_idle_days / owner_node_id).
CREATE TABLE IF NOT EXISTS units (
    unit_id                TEXT PRIMARY KEY,
    tenant_id              TEXT NOT NULL,
    state                  TEXT NOT NULL DEFAULT 'active',
    index_type             TEXT NOT NULL DEFAULT 'hnsw',
    sealed_at              INTEGER DEFAULT NULL,
    archived_at            INTEGER DEFAULT NULL,
    vector_count           INTEGER NOT NULL DEFAULT 0,
    size_bytes             INTEGER NOT NULL DEFAULT 0,
    last_write_at          INTEGER NOT NULL DEFAULT 0,
    seal_threshold_vectors BIGINT  NOT NULL DEFAULT 9223372036854775807,
    seal_threshold_bytes   BIGINT  NOT NULL DEFAULT 9223372036854775807,
    seal_idle_days         INTEGER NOT NULL DEFAULT 2147483647,
    owner_node_id          TEXT NOT NULL DEFAULT 'local',
    created_at             INTEGER NOT NULL,
    config_json            JSONB DEFAULT NULL,
    FOREIGN KEY (tenant_id) REFERENCES tenants(tenant_id)
);

-- 4. nodes (Phase 1: single 'local' row).
CREATE TABLE IF NOT EXISTS nodes (
    node_id        TEXT PRIMARY KEY,
    hostname       TEXT,
    last_heartbeat INTEGER,
    created_at     INTEGER NOT NULL
);
INSERT OR IGNORE INTO nodes (node_id, hostname, created_at)
    VALUES ('local', 'localhost', CAST(strftime('%s','now') AS INTEGER) * 1000);

-- 5. tenants (OPEN-1 dedup_scope config).
CREATE TABLE IF NOT EXISTS tenants (
    tenant_id              TEXT PRIMARY KEY,
    name                   TEXT,
    type                   TEXT,
    dedup_scope            TEXT NOT NULL DEFAULT 'tenant',
    cross_tenant_whitelist JSONB DEFAULT NULL,
    created_at             INTEGER NOT NULL
);

-- 6. namespaces (OPEN-1 isolation + relationship model + 11 *_config JSONB).
CREATE TABLE IF NOT EXISTS namespaces (
    ns_id              TEXT PRIMARY KEY,
    name               TEXT,
    tenant_id          TEXT NOT NULL REFERENCES tenants(tenant_id),
    isolation_mode     TEXT NOT NULL DEFAULT 'shared',
    visibility         TEXT NOT NULL DEFAULT 'tenant',
    owner_user_id      TEXT,
    cloned_from_ns_id  TEXT,
    clone_type         TEXT,
    cloned_at          INTEGER,
    clone_lineage      JSONB DEFAULT NULL,
    status             TEXT NOT NULL DEFAULT 'active',
    created_at         INTEGER NOT NULL,
    -- 11 standardized *_config JSONB columns (v1.0.8: 9 → 11). Default '{}' =
    -- disabled (the spec's '{}'::jsonb cast is Postgres-only; plain '{}' here).
    reranker_config    JSONB DEFAULT '{}',
    enricher_config    JSONB DEFAULT '{}',
    parser_config      JSONB DEFAULT '{}',
    memory_config      JSONB DEFAULT '{}',
    watcher_config     JSONB DEFAULT '{}',
    cleaning_config    JSONB DEFAULT '{}',
    rag_fusion_config  JSONB DEFAULT '{}',
    crag_config        JSONB DEFAULT '{}',
    complexity_config  JSONB DEFAULT '{}',
    sparse_config      JSONB DEFAULT '{}',
    doc_summary_config JSONB DEFAULT '{}'
);

-- ===== 6 association tables =====

-- catalog.content_refs (file-level cross-NS/Unit ref counting; OPEN-2 GC).
-- Same name as per-Unit SQLite.content_refs but different grain (file vs chunk).
CREATE TABLE IF NOT EXISTS content_refs (
    file_hash      TEXT NOT NULL,
    ns_id          TEXT NOT NULL,
    doc_id         TEXT NOT NULL,
    target_unit_id TEXT NOT NULL DEFAULT '',
    status         TEXT NOT NULL DEFAULT 'active',
    created_at     INTEGER NOT NULL,
    PRIMARY KEY (file_hash, ns_id, doc_id)
);
CREATE INDEX IF NOT EXISTS idx_cr_ns ON content_refs(ns_id);

-- blob_gc_queue (OPEN-2 GC Stage 3; ARCH §5.x A6 §10.7 authoritative schema).
-- PK = blob_uri (one file_hash can map to several blob_uris: cortrix-local + s3
-- + …). Stage 2 hard-delete enqueues; the background GC thread scans eligible
-- rows (eligible_at <= now AND status='pending') and unlinks them in Stage 3.
CREATE TABLE IF NOT EXISTS blob_gc_queue (
    blob_uri        TEXT PRIMARY KEY,
    file_hash       TEXT NOT NULL,
    queued_at       INTEGER NOT NULL,
    eligible_at     INTEGER NOT NULL,    -- queued_at + blob_gc_retention_days (default 90d)
    last_ref_check  INTEGER,             -- last time ref_count=0 was reconfirmed (incremental recheck)
    status          TEXT NOT NULL DEFAULT 'pending'  -- pending / unlinking / unlinked
);
CREATE INDEX IF NOT EXISTS idx_gc_eligible ON blob_gc_queue(eligible_at, status);

-- cross_tenant_whitelist (V2.0 enabled; V1 schema reserved).
CREATE TABLE IF NOT EXISTS cross_tenant_whitelist (
    tenant_id_from TEXT NOT NULL,
    tenant_id_to   TEXT NOT NULL,
    created_at     INTEGER NOT NULL,
    PRIMARY KEY (tenant_id_from, tenant_id_to)
);

-- users / user_tenants (relationship model, 2026-04-30 decision).
CREATE TABLE IF NOT EXISTS users (
    user_id      TEXT PRIMARY KEY,
    email        TEXT,
    created_at   INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS user_tenants (
    user_id      TEXT NOT NULL,
    tenant_id    TEXT NOT NULL,
    role         TEXT NOT NULL,   -- 'owner' / 'admin' / 'member' / 'viewer'
    created_at   INTEGER NOT NULL,
    PRIMARY KEY (user_id, tenant_id)
);

-- ns_acl (cross-Tenant shared ACL; ARCH §2.9 6-column authoritative schema).
-- grantee_user_id NULL = whole grantee_tenant_id is granted. SQLite allows NULL
-- in a PK and treats multiple NULLs as distinct rows (ARCH §2.4 compat note).
CREATE TABLE IF NOT EXISTS ns_acl (
    ns_id                TEXT NOT NULL,
    grantee_tenant_id    TEXT NOT NULL,
    grantee_user_id      TEXT,
    role                 TEXT NOT NULL,   -- 'viewer' / 'editor' / 'admin'
    granted_at           INTEGER NOT NULL,
    granted_by_user_id   TEXT,
    PRIMARY KEY (ns_id, grantee_tenant_id, grantee_user_id)
);

-- ===== V1.0 OSS bootstrap rows (P09 §10.1, idempotent) =====
-- Single-tenant OSS runs as 'default_tenant'; namespaces.tenant_id is NOT NULL
-- with an FK (foreign_keys=ON), so these placeholders must exist before the
-- first CreateNamespace. INSERT OR IGNORE keeps restarts idempotent.
INSERT OR IGNORE INTO users (user_id, email, created_at)
    VALUES ('default_user', 'default@cortrix.local', CAST(strftime('%s','now') AS INTEGER) * 1000);
INSERT OR IGNORE INTO tenants (tenant_id, type, name, created_at)
    VALUES ('default_tenant', 'personal', 'Default Tenant', CAST(strftime('%s','now') AS INTEGER) * 1000);
INSERT OR IGNORE INTO user_tenants (user_id, tenant_id, role, created_at)
    VALUES ('default_user', 'default_tenant', 'owner', CAST(strftime('%s','now') AS INTEGER) * 1000);
INSERT OR IGNORE INTO tenants (tenant_id, type, name, created_at)
    VALUES ('__system__', 'system', 'Platform System Tenant', CAST(strftime('%s','now') AS INTEGER) * 1000);
)SQL";

Status CatalogSchemaProvider::Migrate(sqlite3* db, int /*from_ver*/, int /*to_ver*/) {
    // Phase 1: single-step creation from scratch (v0 → v1). Phase 2 internal
    // evolution will branch on (from_ver, to_ver) here.
    //
    // blob_gc_queue clean-break (OPEN-2 / D3.5 r2): the pre-GC MVP image shipped
    // a stale (blob_id/stage/enqueued_at/purge_after) shape with zero consumers.
    // CREATE IF NOT EXISTS keeps the stale table alive on an upgraded data dir
    // and Stage 3 then fails with "no such column: blob_uri". Pre-release the
    // queue holds no durable data (MVP_MIGRATION_POLICY clean-break), so a
    // stale shape is dropped and recreated by the DDL below.
    {
        sqlite3_stmt* probe = nullptr;
        bool has_blob_uri = false;
        bool table_exists = false;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(blob_gc_queue)", -1, &probe,
                               nullptr) == SQLITE_OK) {
            while (sqlite3_step(probe) == SQLITE_ROW) {
                table_exists = true;
                const unsigned char* col = sqlite3_column_text(probe, 1);
                if (col != nullptr &&
                    std::string(reinterpret_cast<const char*>(col)) == "blob_uri") {
                    has_blob_uri = true;
                }
            }
            sqlite3_finalize(probe);
        }
        if (table_exists && !has_blob_uri) {
            sqlite3_exec(db, "DROP TABLE blob_gc_queue", nullptr, nullptr, nullptr);
        }
    }
    char* err = nullptr;
    if (sqlite3_exec(db, kCatalogSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        return Status::Internal(std::string("catalog schema migration failed: ") + msg);
    }
    return Status::Ok();
}

}  // namespace cortrix::catalog
