#pragma once
// [D3.5-B] Single source of truth for the per-Unit framework schema
// (documents / blocks / blocks_fts + indices + triggers).
//
// Used by BOTH paths so they build byte-identical schema:
//   - production: F09SchemaProvider::Migrate, run via SchemaMigrator::MigrateUnit
//     at LoadOneNamespace (unified schema governance);
//   - standalone/test: CortrixStoreSqlite::Open in owns-db mode (no migrator).
//
// The DDL below was moved verbatim out of CortrixStoreSqlite::CreateTables — keep
// it the single owner; do not duplicate the statements elsewhere.

namespace cortrix::store {

inline constexpr char kPerUnitFrameworkDdl[] = R"SQL(
        CREATE TABLE IF NOT EXISTS documents (
            doc_id          TEXT PRIMARY KEY,                  -- ULID (D-I6); app-minted, no AUTOINCREMENT
            source_type     TEXT NOT NULL,
            source_path     TEXT NOT NULL,
            source_ref      TEXT,
            content_hash    TEXT,
            file_size       INTEGER,
            mime_type       TEXT,
            status          TEXT NOT NULL DEFAULT 'pending',
            error_message   TEXT,
            processing_level INTEGER NOT NULL DEFAULT 3,
            block_count     INTEGER DEFAULT 0,
            chunk_strategy  TEXT,
            created_at      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
            updated_at      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
            processed_at    TEXT,
            cdc_schema      TEXT,
            cdc_position    TEXT,
            cdc_last_sync   TEXT,
            title           TEXT,
            language        TEXT,
            metadata_json   TEXT,
            deleted_at      INTEGER                            -- OPEN-2 Stage 1 soft-delete timestamp (ms); NULL = live
        );
        CREATE INDEX IF NOT EXISTS idx_doc_source ON documents(source_type, source_path);
        CREATE INDEX IF NOT EXISTS idx_doc_status ON documents(status);
        CREATE INDEX IF NOT EXISTS idx_doc_hash   ON documents(content_hash) WHERE content_hash IS NOT NULL;
        -- OPEN-2 Stage 2: efficiently find soft-deleted docs past their retention window.
        CREATE INDEX IF NOT EXISTS idx_doc_deleted_at ON documents(deleted_at) WHERE deleted_at IS NOT NULL;

        CREATE TABLE IF NOT EXISTS blocks (
            block_id        INTEGER PRIMARY KEY,  -- app-provided uint64 = HashChildIdToBlockId (D3.5 wire⑤); rowid fallback when 0

            doc_id          TEXT NOT NULL,
            chunk_index     INTEGER NOT NULL,
            block_type      INTEGER NOT NULL,
            processing_level INTEGER NOT NULL,
            hnsw_node_id    INTEGER,
            content_hash    BLOB,
            data            BLOB NOT NULL,
            content_text    TEXT,
            -- [A unified-blocks #4] Queryable JSONB metadata column,
            -- shared by all block_types: MEM memory state (block_type=MEMORY, MEM02
            -- §4.2.1 jsonb_set/JSONB index), F34 child metadata (inherited NER/Summary),
            -- F08 META 26-field doc metadata. Three-way split: data=binary F09 block
            -- (reconstruction), metadata_json=queryable JSON, content_text=full-text.
            metadata_json   TEXT,
            FOREIGN KEY (doc_id) REFERENCES documents(doc_id)
        );
        CREATE INDEX IF NOT EXISTS idx_block_doc  ON blocks(doc_id, chunk_index);
        CREATE INDEX IF NOT EXISTS idx_block_type ON blocks(block_type);
        CREATE INDEX IF NOT EXISTS idx_block_hnsw ON blocks(hnsw_node_id) WHERE hnsw_node_id IS NOT NULL;
        CREATE INDEX IF NOT EXISTS idx_block_hash ON blocks(content_hash) WHERE content_hash IS NOT NULL;

        CREATE VIRTUAL TABLE IF NOT EXISTS blocks_fts USING fts5(
            content_text,
            content='blocks',
            content_rowid='block_id'
        );

        CREATE TRIGGER IF NOT EXISTS blocks_ai AFTER INSERT ON blocks
        WHEN NEW.content_text IS NOT NULL BEGIN
            INSERT INTO blocks_fts(rowid, content_text)
            VALUES (NEW.block_id, NEW.content_text);
        END;

        CREATE TRIGGER IF NOT EXISTS blocks_ad AFTER DELETE ON blocks
        WHEN OLD.content_text IS NOT NULL BEGIN
            INSERT INTO blocks_fts(blocks_fts, rowid, content_text)
            VALUES('delete', OLD.block_id, OLD.content_text);
        END;

        CREATE TRIGGER IF NOT EXISTS blocks_au AFTER UPDATE OF content_text ON blocks
        WHEN OLD.content_text IS NOT NULL OR NEW.content_text IS NOT NULL BEGIN
            INSERT INTO blocks_fts(blocks_fts, rowid, content_text)
            SELECT 'delete', OLD.block_id, OLD.content_text
            WHERE OLD.content_text IS NOT NULL;
            INSERT INTO blocks_fts(rowid, content_text)
            SELECT NEW.block_id, NEW.content_text
            WHERE NEW.content_text IS NOT NULL;
        END;
    )SQL";

}  // namespace cortrix::store
