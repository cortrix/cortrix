#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::spc {

/// The document parser's schema-migration contribution. Adds the
/// `namespaces.parser_config` JSONB (TEXT in SQLite) column with default
/// '{}' so a namespace can override parser options (max_pages /
/// max_file_size_mb / enable_ocr_fallback / language_hint /
/// subprocess_timeout_ms whitelist). The parser owns no per-Unit schema (parse
/// output flows straight to the chunker), so only the catalog migration is meaningful.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider (3
/// methods). Registered with the SchemaMigrator at Engine::Init() in the
/// topological order (catalog → parser → ...). The ADD COLUMN is idempotent (guarded
/// by a table_info check) so a re-run / already-current DB is a no-op.
class F06SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "F06"; }
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): add namespaces.parser_config if absent.
    /// Idempotent. Any other (from, to) step is a version mismatch until a
    /// future schema bump implements it.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::spc
