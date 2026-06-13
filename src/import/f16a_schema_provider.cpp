#include "cortrix/import/f16a_schema_provider.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::import {

// F16a schema — db_connections (§4.1) + import_tasks (§4.2). Kept as one DDL batch
// so the migrator applies it atomically. SQLite dialect (see header dialect note):
// the spec's BIGSERIAL → INTEGER PRIMARY KEY AUTOINCREMENT, TIMESTAMP → INTEGER
// (Unix ms), now() → strftime('%s','now')*1000, JSONB → TEXT (TEXT affinity). FK
// targets tenants(tenant_id) / namespaces(ns_id) are catalog.db TEXT PKs (F12
// catalog SoT — V6 Stage 3 A32 aligned both to TEXT). IF NOT EXISTS everywhere so a
// re-run on an already-migrated db is a no-op outside the migrator's version gate.
const char* const kF16aSchemaSql = R"SQL(
-- db_connections (F16a §4.1, D1): pre-registered DB credential refs. Stores only an
-- encrypted secret reference + non-sensitive metadata, never the raw DSN.
CREATE TABLE IF NOT EXISTS db_connections (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    ref_id        TEXT UNIQUE NOT NULL,                          -- "db_conn_<ulid>"
    name          TEXT NOT NULL,                                 -- business-readable name
    tenant_id     TEXT NOT NULL REFERENCES tenants(tenant_id),   -- F12 catalog SoT (TEXT PK)

    -- encrypted credential: a key id into the secret store, NOT the DSN itself.
    secret_key_id TEXT NOT NULL,

    -- non-sensitive metadata (no password).
    host          TEXT NOT NULL,
    db_name       TEXT NOT NULL,
    registered_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    registered_by TEXT NOT NULL,                                 -- user_id

    -- 30d expiry (D1 ref-rotation mechanism), Unix ms.
    expires_at    INTEGER NOT NULL,

    -- revocation.
    revoked_at    INTEGER,
    revoked_by    TEXT,
    revoke_reason TEXT
);

CREATE INDEX IF NOT EXISTS idx_db_connections_tenant ON db_connections(tenant_id);
CREATE INDEX IF NOT EXISTS idx_db_connections_active ON db_connections(tenant_id, expires_at)
    WHERE revoked_at IS NULL;

-- import_tasks (F16a §4.2, D6): one row per async import task (mimics the F42 task
-- table shape, but is F16a-owned — no dependency on F42's schema).
CREATE TABLE IF NOT EXISTS import_tasks (
    id                    INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id               TEXT UNIQUE NOT NULL,                        -- "import_<ulid>"
    ns_id                 TEXT NOT NULL REFERENCES namespaces(ns_id),  -- F12 catalog SoT (TEXT PK)
    tenant_id             TEXT NOT NULL REFERENCES tenants(tenant_id), -- F12 catalog SoT (TEXT PK)

    -- request parameters.
    connection_ref_id     TEXT NOT NULL,                               -- references db_connections.ref_id
    request_json          TEXT NOT NULL,                               -- D2 dual-mode params (JSON)
    text_strategy         TEXT NOT NULL,                               -- D4: per_row / merge

    -- state.
    status                TEXT NOT NULL DEFAULT 'queued',              -- queued/running/completed/failed/cancelling/cancelled
    progress              REAL NOT NULL DEFAULT 0.0,                   -- 0.0 - 1.0
    rows_imported         INTEGER NOT NULL DEFAULT 0,
    rows_total            INTEGER,
    rows_failed           INTEGER NOT NULL DEFAULT 0,

    -- timestamps (Unix ms).
    queued_at             INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    started_at            INTEGER,
    completed_at          INTEGER,
    estimated_completion_at INTEGER,

    -- error (CX_ERR_F16A_*).
    error_code            TEXT,
    error_message         TEXT,
    error_structured_data TEXT,

    -- operator.
    submitted_by          TEXT NOT NULL                                -- user_id
);

CREATE INDEX IF NOT EXISTS idx_import_tasks_ns_status ON import_tasks(ns_id, status);
CREATE INDEX IF NOT EXISTS idx_import_tasks_running ON import_tasks(status, queued_at)
    WHERE status IN ('queued', 'running');
)SQL";

Status F16aSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 single-step creation. Accept an already-current (1 → 1) call
    // defensively. Phase 2 internal evolution will branch on (from_ver, to_ver).
    if (from_ver == to_ver) {
        return Status::Ok();  // already current
    }
    if (from_ver == 0 && to_ver == kF16aSchemaVersion) {
        char* err = nullptr;
        if (sqlite3_exec(db, kF16aSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "sqlite error";
            sqlite3_free(err);
            return Status::Internal(
                std::string("CX_ERR_F16A_SCHEMA: db_connections/import_tasks migration failed: ") + msg);
        }
        return Status::Ok();
    }
    // Unexpected version step (e.g. Phase 2 1 → 2) is not implemented yet.
    return Status::InvalidArgument(
        "CX_ERR_SCHEMA_VERSION_MISMATCH: F16a unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::import
