-- Async task Document Async Processing — tasks table (detailed design).
--
-- Async-scheduling metadata, decoupled from the catalog blob_gc_queue:
-- platform-level, applied by cortrix::async::TaskManager::Init at runtime
-- (CREATE TABLE IF NOT EXISTS). This file is the canonical schema artifact and
-- MUST stay byte-identical to TaskManager::CreateTasksTable() DDL.
--
-- SQLite dialect: BOOLEAN → INTEGER 0/1; JSON columns → TEXT (failed_pages,
-- structured_data). status CHECK enforces the state machine:
--   queued → processing → completed
--                      ↘ failed
--          ↗ cancelling → cancelled

CREATE TABLE IF NOT EXISTS tasks (
    task_id          TEXT PRIMARY KEY,
    namespace_id     TEXT NOT NULL,
    filename         TEXT NOT NULL,
    filepath         TEXT NOT NULL,                  -- temporary file path
    doc_id           TEXT,                           -- filled in after completion (Phase 1: inferred from content_hash)
    content_hash     TEXT,                           -- topic 2.2 — used by the Watcher for debounce dedup
    status           TEXT NOT NULL DEFAULT 'queued', -- queued|processing|cancelling|completed|failed|cancelled
    task_type        INTEGER NOT NULL DEFAULT 1,     -- async::TaskType (1=DocParse,2=WatcherFanout,3=DocSummary)
    cancel_requested INTEGER NOT NULL DEFAULT 0,     -- topic 3 — 0/1 cancel-request flag
    total_pages      INTEGER DEFAULT 0,
    processed_pages  INTEGER DEFAULT 0,
    failed_pages     TEXT DEFAULT '[]',              -- JSON array of int
    progress_pct     REAL DEFAULT 0.0,
    eta_seconds      INTEGER DEFAULT -1,
    current_phase    TEXT,                           -- topic 5 — parsing|enriching|indexing
    worker_id        INTEGER,                        -- topic 5 — which Worker is processing it
    trace_id         TEXT,                           -- topic 6 — cross-Worker trace correlation
    error_code       TEXT,                           -- topic 5 — GEN-Agent CX_ERR_* code
    error_msg        TEXT,
    structured_data  TEXT,                           -- topic 5 — GEN-Agent JSON error context
    created_at       TEXT NOT NULL,                  -- queued time
    updated_at       TEXT NOT NULL,
    started_at       TEXT,                           -- actual processing-start time
    completed_at     TEXT,
    CHECK (status IN ('queued','processing','cancelling','completed','failed','cancelled'))
);

CREATE INDEX IF NOT EXISTS idx_tasks_namespace  ON tasks(namespace_id);
CREATE INDEX IF NOT EXISTS idx_tasks_status     ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_tasks_doc_id     ON tasks(doc_id);          -- topic 2.3 dequeue mutual exclusion
CREATE INDEX IF NOT EXISTS idx_tasks_created_at ON tasks(created_at);      -- topic 4 cron cleanup
