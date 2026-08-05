#include "cortrix/agent_trace/interaction_sources_schema.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::agent_trace {

// interaction_sources schema (Open-Core split).
// CE table, no highlight ranges. One DDL batch applied atomically by the migrator;
// IF NOT EXISTS on every object for defensive idempotency. interaction_id is TEXT
// to match the real interaction_log.id (UUID), with ON DELETE CASCADE.
const char* const kInteractionSourcesSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS interaction_sources (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    interaction_id  TEXT NOT NULL,               -- FK -> interaction_log.id (UUID, real frozen PK)
    source_block_id TEXT NOT NULL,               -- -> blocks table
    source_type     VARCHAR(16),                 -- "block" / "memory" / "metadata"
    relevance_score REAL,
    snippet         TEXT,                         -- §6 -- <=500 chars, keep first 400 + last 100

    FOREIGN KEY (interaction_id) REFERENCES interaction_log(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_sources_interaction ON interaction_sources(interaction_id);
CREATE INDEX IF NOT EXISTS idx_sources_block       ON interaction_sources(source_block_id);
)SQL";

Status InteractionSourcesSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    if (from_ver == to_ver) {
        return Status::Ok();  // already current
    }
    if (from_ver == 0 && to_ver == kInteractionSourcesSchemaVersion) {
        char* err = nullptr;
        if (sqlite3_exec(db, kInteractionSourcesSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "sqlite error";
            sqlite3_free(err);
            return Status::Internal(
                std::string("CX_ERR_TRACE_INTERNAL: F13 interaction_sources schema migration failed: ") + msg);
        }
        return Status::Ok();
    }
    return Status::InvalidArgument(
        "CX_ERR_TRACE_INTERNAL: F13 interaction_sources unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::agent_trace
