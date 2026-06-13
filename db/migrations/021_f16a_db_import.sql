-- F16a DB Manual Import — db_connections (§4.1) + import_tasks (§4.2)
-- Wave B-R2. Lives in the catalog DB (catalog.db) alongside tenants / namespaces.
--
-- This file is the human-readable mirror of the DDL the F16aSchemaProvider emits
-- (src/import/f16a_schema_provider.cpp kF16aSchemaSql). The provider is the
-- runtime SoT (applied atomically via the F12 SchemaMigrator, after F12 so the
-- tenants/namespaces FK targets exist); this file documents the same schema for
-- review / ops inspection. Keep the two in lock-step.
--
-- Dialect: SQLite (catalog DB). The F16a §4.x spec is written in Postgres syntax
-- (BIGSERIAL / TIMESTAMP / now()); transcribed here to the SQLite idiom every
-- other catalog provider uses: INTEGER PRIMARY KEY AUTOINCREMENT, Unix-ms INTEGER
-- timestamps, JSONB → TEXT affinity, IF NOT EXISTS on every object.

-- db_connections (D1): pre-registered DB credential refs. Stores only an encrypted
-- secret reference + non-sensitive metadata, never the raw DSN.
CREATE TABLE IF NOT EXISTS db_connections (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    ref_id        TEXT UNIQUE NOT NULL,                          -- "db_conn_<ulid>"
    name          TEXT NOT NULL,                                 -- business-readable name
    tenant_id     TEXT NOT NULL REFERENCES tenants(tenant_id),   -- F12 catalog SoT (TEXT PK)

    secret_key_id TEXT NOT NULL,                                 -- key id into the secret store, NOT the DSN

    host          TEXT NOT NULL,                                 -- non-sensitive metadata (no password)
    db_name       TEXT NOT NULL,
    registered_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    registered_by TEXT NOT NULL,                                 -- user_id

    expires_at    INTEGER NOT NULL,                              -- 30d expiry (Unix ms)

    revoked_at    INTEGER,                                       -- revocation
    revoked_by    TEXT,
    revoke_reason TEXT
);

CREATE INDEX IF NOT EXISTS idx_db_connections_tenant ON db_connections(tenant_id);
CREATE INDEX IF NOT EXISTS idx_db_connections_active ON db_connections(tenant_id, expires_at)
    WHERE revoked_at IS NULL;

-- import_tasks (D6): one row per async import task (mimics the F42 task table shape,
-- but is F16a-owned — no dependency on F42's schema).
CREATE TABLE IF NOT EXISTS import_tasks (
    id                    INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id               TEXT UNIQUE NOT NULL,                        -- "import_<ulid>"
    ns_id                 TEXT NOT NULL REFERENCES namespaces(ns_id),  -- F12 catalog SoT (TEXT PK)
    tenant_id             TEXT NOT NULL REFERENCES tenants(tenant_id), -- F12 catalog SoT (TEXT PK)

    connection_ref_id     TEXT NOT NULL,                               -- references db_connections.ref_id
    request_json          TEXT NOT NULL,                               -- D2 dual-mode params (JSON)
    text_strategy         TEXT NOT NULL,                               -- D4: per_row / merge

    status                TEXT NOT NULL DEFAULT 'queued',              -- queued/running/completed/failed/cancelling/cancelled
    progress              REAL NOT NULL DEFAULT 0.0,                   -- 0.0 - 1.0
    rows_imported         INTEGER NOT NULL DEFAULT 0,
    rows_total            INTEGER,
    rows_failed           INTEGER NOT NULL DEFAULT 0,

    queued_at             INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    started_at            INTEGER,
    completed_at          INTEGER,
    estimated_completion_at INTEGER,

    error_code            TEXT,                                        -- CX_ERR_F16A_*
    error_message         TEXT,
    error_structured_data TEXT,

    submitted_by          TEXT NOT NULL                                -- user_id
);

CREATE INDEX IF NOT EXISTS idx_import_tasks_ns_status ON import_tasks(ns_id, status);
CREATE INDEX IF NOT EXISTS idx_import_tasks_running ON import_tasks(status, queued_at)
    WHERE status IN ('queued', 'running');
