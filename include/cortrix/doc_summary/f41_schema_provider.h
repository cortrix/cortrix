#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::doc_summary {

/// Canonical F41 §4.3 doc-level FTS5 DDL — the single SoT for the
/// `doc_fts5_index` virtual table, shared by F41SchemaProvider::Migrate (the D3.5
/// integrated path) and DocFts5Index::Open (the standalone self-contained path)
/// so the two can never drift.
///
/// The index content = F08 fields (filename / doc_title / topics_rule_extracted /
/// authors), NOT doc_summary (§4.3: the LLM summary lives in the doc_summary Block
/// embedding via P-HNSW, not in this FTS5 table). doc_id is UNINDEXED (returned,
/// not searched). This is per-Unit and distinct from F09's chunk-level blocks_fts
/// (different table, no conflict — F41 §4.3).
inline constexpr const char* kDocFts5IndexDdl = R"SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS doc_fts5_index USING fts5(
    doc_id UNINDEXED,
    filename,
    doc_title,
    topics_rule_extracted,
    authors,
    tokenize='unicode61'
);
)SQL";

/// F41's schema-migration contribution. Owns the net-new doc-level
/// `doc_fts5_index` FTS5 virtual table (hybrid fallback, F41-8). The doc_summary
/// Block itself reuses the F09 blocks table (block_type=17), so F41 adds no
/// regular table — only this per-Unit virtual table.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider; the
/// v1.0.3 reverse revision (V9 F1 ARCH cascade) unified the old MigrateCatalog +
/// MigrateUnit pair into the single Migrate(sqlite3*, int, int). Migrate returns
/// Status (F-FREEZE-1 / CODING_CONVENTIONS §3), runs inside the SchemaMigrator's
/// transaction.
///
/// Standalone (D3): registering this with the live CatalogDb/Unit migrator at
/// bootstrap is cross-Feature wiring → D3.5. DocFts5Index::Open() creates the same
/// virtual table self-contained for standalone tests; this provider is the
/// integrated, versioned path that runs against the F05 Unit DB at D3.5.
class F41SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// F12 registration key (aligns with F40/F09/F38 SchemaProvider naming).
    std::string FeatureName() const override { return "F41"; }

    /// V1 = the doc_fts5_index virtual table. Phase 2 (§15) would bump this.
    int CurrentVersion() const override { return 1; }

    /// from_ver 0 → 1: CREATE the doc_fts5_index FTS5 virtual table. An
    /// already-current (1 → 1) call is accepted defensively (CREATE ... IF NOT
    /// EXISTS is idempotent). Any other (from, to) is a version mismatch
    /// (CX_ERR_F41_SCHEMA_VERSION_MISMATCH) until a Phase-2 step is defined.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::doc_summary
