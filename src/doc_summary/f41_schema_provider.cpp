#include "cortrix/doc_summary/f41_schema_provider.h"

#include <sqlite3.h>

#include <string>

#include "cortrix/doc_summary/doc_summary_error.h"

namespace cortrix::doc_summary {

Status F41SchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 creates the doc_fts5_index virtual table (F41 §4.3). Accept
    // an already-current (1 → 1) call defensively (CREATE ... IF NOT EXISTS is
    // itself idempotent). Runs inside the SchemaMigrator's transaction.
    if (from_ver == to_ver) {
        return Status::Ok();  // already current
    }
    if (from_ver == 0 && to_ver == 1) {
        if (db == nullptr) {
            return DocSummaryStatus(DocSummaryErrorCode::kSchemaVersionMismatch,
                                    "F41 Migrate got null db");
        }
        char* err = nullptr;
        int rc = sqlite3_exec(db, kDocFts5IndexDdl, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "DDL failed";
            sqlite3_free(err);
            return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                    "create doc_fts5_index: " + msg);
        }
        return Status::Ok();
    }
    // Phase 2 (§15) is the only future step; until it is defined an unexpected
    // version jump is an error.
    return DocSummaryStatus(
        DocSummaryErrorCode::kSchemaVersionMismatch,
        "F41 unsupported migration " + std::to_string(from_ver) + " -> " +
            std::to_string(to_ver));
}

}  // namespace cortrix::doc_summary
