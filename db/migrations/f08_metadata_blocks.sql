-- Metadata Block — metadata_blocks table (detailed design §3.3, D5 lock: separate table).
--
-- Per-Unit document-level metadata block storage (Chunk[0]): 1 doc = 1 row. Applied
-- by cortrix::metadata::MetadataSchemaProvider::Migrate at runtime (CREATE TABLE IF
-- NOT EXISTS). This file is the canonical schema artifact and MUST stay byte-identical
-- to kMetadataSchemaSql (src/metadata/metadata_schema_provider.cpp).
--
-- SQLite dialect: created_at as Unix-ms INTEGER; metadata_json as TEXT (26-field JSON,
-- D2/D9 + V2 remediation, incl. custom_metadata + meta.parse_status + meta.parse_failed_page).
--
-- FK reconciliation (D3.5 deferred): detailed design §3.3 declares
--   FOREIGN KEY (doc_id) REFERENCES documents(doc_id)
-- but the per-Unit documents.doc_id is INTEGER while META block's doc_id is the TEXT ULID
-- (id/types.h). That type mismatch is a cross-feature schema-authority question with
-- no MVP_MIGRATION_POLICY ruling → wiring the FK belongs to real pipeline integration,
-- not a standalone unilateral fix. The column stays TEXT UNIQUE; the FK is omitted here.

CREATE TABLE IF NOT EXISTS metadata_blocks (
    block_id      TEXT PRIMARY KEY,                    -- ULID (id/ulid.h)
    doc_id        TEXT NOT NULL UNIQUE,                -- 1 doc = 1 metadata block
    namespace_id  TEXT NOT NULL,
    block_text    TEXT NOT NULL,                       -- D3 natural-language sentence (embedding input)
    metadata_json TEXT NOT NULL,                       -- D2/D9 + V2 remediation: complete 26-field JSON
    created_at    INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);

CREATE INDEX IF NOT EXISTS idx_metablocks_ns ON metadata_blocks(namespace_id, created_at);
