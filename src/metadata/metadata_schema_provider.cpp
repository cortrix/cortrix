#include "cortrix/metadata/metadata_schema_provider.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::metadata {

// Metadata schema — metadata_blocks (separate table). SQLite dialect (same idiom as
// every other provider): created_at as Unix-ms INTEGER, metadata_json as TEXT
// affinity, IF NOT EXISTS so a re-run on an already-migrated db is a no-op outside the
// migrator's version gate. doc_id is UNIQUE (1 doc = 1 metadata block). The FK to
// documents(doc_id) is intentionally omitted standalone — see the header's
// "FK reconciliation (D3.5 deferred)" note (documents.doc_id is INTEGER vs the metadata block's TEXT
// ULID; reconciling the type + wiring the FK is real pipeline work).
const char* const kMetadataSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS metadata_blocks (
    block_id      TEXT PRIMARY KEY,                    -- ULID (id/ulid.h)
    doc_id        TEXT NOT NULL UNIQUE,                -- 1 doc = 1 metadata block
    namespace_id  TEXT NOT NULL,
    block_text    TEXT NOT NULL,                       -- D3 natural-language sentence (embedding input)
    metadata_json TEXT NOT NULL,                       -- D2/D9 + V2 remediation: complete 26-field JSON
    created_at    INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
    -- D9 B' compromise: V1.0 immutable schema; Phase 2 adds block_version / updated_at
    --             + F08 Block Versioning + Update API Feature.
    -- D3.5: FOREIGN KEY (doc_id) REFERENCES documents(doc_id) — pending doc_id type
    --       reconciliation (documents.doc_id INTEGER vs F08 TEXT ULID).
);

CREATE INDEX IF NOT EXISTS idx_metablocks_ns ON metadata_blocks(namespace_id, created_at);
)SQL";

Status MetadataSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 single-step creation. Accept an already-current (1 → 1) call
    // defensively. Phase 2 internal evolution will branch on (from_ver, to_ver).
    if (from_ver == to_ver) {
        return Status::Ok();  // already current
    }
    if (from_ver == 0 && to_ver == kMetadataSchemaVersion) {
        char* err = nullptr;
        if (sqlite3_exec(db, kMetadataSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "sqlite error";
            sqlite3_free(err);
            return Status::Internal(
                std::string("CX_ERR_METADATA_SCHEMA: metadata_blocks migration failed: ") + msg);
        }
        return Status::Ok();
    }
    // Unexpected version step (e.g. Phase 2 1 → 2) is not implemented yet.
    return Status::InvalidArgument(
        "CX_ERR_SCHEMA_VERSION_MISMATCH: F08 unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::metadata
