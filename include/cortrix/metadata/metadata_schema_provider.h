#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/status.h"

namespace cortrix::metadata {

/// Current metadata schema version. Phase 1 single-step create (v0 → v1);
/// Phase 2 (Block Versioning + Update API) adds block_version / updated_at and
/// will branch on (from, to).
constexpr int kMetadataSchemaVersion = 1;

/// The DDL emitted by the schema provider (metadata_blocks table +
/// idx_metablocks_ns). Lives in the per-Unit DB alongside the documents / blocks
/// tables (a separate metadata_blocks table — does not pollute parents/children), applied via
/// the shared SchemaMigrator so it runs inside the same versioned, atomic framework.
///
/// FK reconciliation (D3.5 deferred — surfaced, not silently hacked): detailed design §3.3
/// declares `FOREIGN KEY (doc_id) REFERENCES documents(doc_id)`, but the per-Unit
/// `documents.doc_id` is currently `INTEGER PRIMARY KEY AUTOINCREMENT`
/// (src/store/cortrix_store_sqlite.cpp) while this doc_id is the TEXT ULID
/// (id/types.h DocId = std::string). That TEXT-vs-INTEGER mismatch is a genuine
/// cross-feature schema-authority question (no MVP_MIGRATION_POLICY entry resolves
/// it) → it belongs to real pipeline wiring, NOT a standalone unilateral fix. So the
/// standalone DDL keeps the column `doc_id TEXT NOT NULL UNIQUE` (1 doc = 1 metadata
/// block, per spec) but does NOT emit the FK constraint; wiring the FK once the
/// documents.doc_id type is reconciled is a D3.5 deferred item.
extern const char* const kMetadataSchemaSql;

/// The metadata ISchemaProvider (frozen cortrix::catalog::ISchemaProvider): owns
/// metadata_blocks + its index in the per-Unit DB. Registered with the per-Unit
/// SchemaMigrator. Migrate returns Status (F-FREEZE-1: no Result<void>).
class MetadataSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "F08"; }
    int CurrentVersion() const override { return kMetadataSchemaVersion; }

    /// Phase 1 (from_ver 0 → 1): create metadata_blocks + idx_metablocks_ns. An
    /// already-current (1 → 1) call is a defensive no-op. Any other step is a version
    /// mismatch until Phase 2.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::metadata
