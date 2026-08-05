#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::observability {

/// Current operation_log schema version. Phase 1 single-step create
/// (v0 → v1). Phase 2 internal evolution branches on (from, to) here.
constexpr int kOplogSchemaVersion = 1;

/// The operation_log DDL emitted by the F18a SchemaProvider:
/// the operation_log table + 5 indices. Stored in cortrix_global.db (the global
/// DB, NOT a per-namespace DB), but applied via the shared SchemaMigrator so it
/// runs inside the same versioned, atomic framework as the catalog providers.
///
/// Open-Core: this is the CE-only schema. Ent's audit_log_extension table is a
/// SEPARATE downstream migration keyed by operation_log.id — it does
/// not alter this table (GEN-OpenCore-Boundary; F18a §4.2).
///
/// SQLite dialect note (cortrix_global.db is SQLite WAL): the §5.1 spec spells
/// trace_id/session_id as VARCHAR(128); SQLite is dynamically typed and accepts
/// VARCHAR(n) as a type name (TEXT affinity, no length enforcement), so the
/// declared type is kept verbatim for spec fidelity. timestamp is Unix ms.
extern const char* const kOperationLogSchemaSql;

/// F18a's ISchemaProvider (frozen cortrix::catalog::ISchemaProvider, D2-pre-5):
/// owns the operation_log table + 5 indices in cortrix_global.db. Registered with
/// the SchemaMigrator that targets the global DB. Migrate returns Status
/// (F-FREEZE-1: no Result<void>).
class OperationLogSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "F18a"; }
    int CurrentVersion() const override { return kOplogSchemaVersion; }

    /// Phase 1 (from_ver 0 → 1): create operation_log + 5 indices. An
    /// already-current (1 → 1) call is a defensive no-op. Any other step is a
    /// version mismatch until Phase 2.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::observability
