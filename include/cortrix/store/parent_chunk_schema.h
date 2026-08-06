#pragma once

// Parents schema DDL.
//
// [A unified-blocks reconcile] Child chunks no longer live in a
// standalone `children` table — they are rows of the unified per-Unit `blocks`
// table (child rows = blocks with child_id IS NOT NULL; block_type = source
// modality), with chunker, contextual and sparse columns ALTER'd onto `blocks` by their per-Unit
// SchemaProviders (ParentChildSchemaProvider owns child_id/parent_id/token_count/
// parent_offset + the 3 indexes; metadata in blocks.metadata_json). See
// the architecture doc documents the durable in-repository model.
//
//   - kParentsSchemaSql  — the `parents` table (parent_text store,: SQLite
//       over Blob for < 10MB/parent + sub-ms single-point lookup, + hotness
//       fields reserved). ParentChildSchemaProvider creates this in the production
//       MigrateUnit path; SqliteParentChunkStore (standalone) also creates it.
//   - kChildrenSchemaSql — LEGACY. The pre-A standalone `children` table, kept ONLY
//       for SqliteParentChunkStore's standalone unit tests until the SPC write path
//       writes children into `blocks` (integration SPC wiring). NOT created by any
//       SchemaProvider; not part of the A unified-blocks model. Slated for removal
//       once SqliteParentChunkStore's write path targets blocks.

namespace cortrix::store {

// parents — kept under A (parent_text reverse-lookup for retrieval expansion).
inline constexpr const char* kParentsSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS parents (
    parent_id    TEXT PRIMARY KEY,
    doc_id       TEXT NOT NULL,
    namespace_id TEXT NOT NULL,
    parent_text  TEXT NOT NULL,
    token_count  INTEGER NOT NULL,
    page_start   INTEGER NOT NULL,
    page_end     INTEGER NOT NULL,
    byte_offset_start INTEGER NOT NULL,
    byte_offset_end   INTEGER NOT NULL,
    metadata_json    TEXT NOT NULL,
    created_at   INTEGER NOT NULL,
    -- reserved (V1.0 neither written nor read, Phase 2 per-parent hotness)
    access_count    INTEGER NOT NULL DEFAULT 0,
    last_access_at  INTEGER,
    hotness_score   REAL NOT NULL DEFAULT 0.0
);
CREATE INDEX IF NOT EXISTS idx_parents_doc ON parents(doc_id, page_start);
CREATE INDEX IF NOT EXISTS idx_parents_ns  ON parents(namespace_id, created_at);
)SQL";

// children — LEGACY (pre-A). See header note. Under A unified-blocks, child chunks
// are rows of `blocks`; this table is retained only for SqliteParentChunkStore's
// standalone path until SPC writes children to blocks (integration).
inline constexpr const char* kChildrenSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS children (
    child_id     TEXT PRIMARY KEY,
    parent_id    TEXT,                         -- nullable: flat-fallback children have none
    doc_id       TEXT NOT NULL,
    namespace_id TEXT NOT NULL,
    child_text   TEXT NOT NULL,
    token_count  INTEGER NOT NULL,
    parent_offset INTEGER NOT NULL,
    chunk_index  INTEGER NOT NULL,
    metadata_json TEXT NOT NULL,
    created_at   INTEGER NOT NULL,
    embedding             BLOB,
    contextualized_text   TEXT,
    contextualized_embedding BLOB,
    contextualized_status SMALLINT DEFAULT 0,
    sparse_vec   BLOB
);
CREATE INDEX IF NOT EXISTS idx_children_parent ON children(parent_id);
CREATE INDEX IF NOT EXISTS idx_children_doc    ON children(doc_id);
CREATE INDEX IF NOT EXISTS idx_children_ns     ON children(namespace_id);
)SQL";

}  // namespace cortrix::store
