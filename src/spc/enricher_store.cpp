#include <cstdint>
#include "cortrix/spc_enricher/enricher_store.h"

#include <nlohmann/json.hpp>

namespace cortrix::spc {

namespace {

Status SqlErr(sqlite3* db, const char* what) {
    return Status::Internal(std::string("CX_ERR_ENRICHER_STORE: ") + what + ": " +
                            (db ? sqlite3_errmsg(db) : "null db"));
}

}  // namespace

Status WriteBlockEnrichment(sqlite3* db, uint64_t block_id, const EnrichResult& result) {
    if (!db) return Status::InvalidArgument("WriteBlockEnrichment: null db");
    // Persist enrichment columns only when an enricher actually ran. Two cases
    // leave the columns NULL (no enrichment, per §3.1):
    //   - a failed result (status != 0);
    //   - a NullEnricher empty-success result — status==0 but enricher_name=="" —
    //     which carries no real score/summary/entities (a 0.0 score there would
    //     falsely read as "enriched, quality 0", so we must NOT write it).
    if (!result.ok() || result.enricher_name.empty()) return Status::Ok();

    // enricher_metadata JSONB = the B-class audit fields (§3.1).
    nlohmann::json meta = {
        {"prompt_version", result.prompt_version},
        {"model_used", result.model_used},
        {"enricher_name", result.enricher_name},
        {"token_count", result.token_count},
        {"duration_ms", result.duration_ms},
    };
    const std::string meta_str = meta.dump();

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE blocks SET enriched_score=?1, enriched_at=?2, enricher_metadata=?3 "
        "WHERE block_id=?4";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare update blocks");
    }
    sqlite3_bind_double(stmt, 1, result.enriched_score);
    sqlite3_bind_int64(stmt, 2, result.enriched_at);
    sqlite3_bind_text(stmt, 3, meta_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, block_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SqlErr(db, "update blocks enrichment");
    return Status::Ok();
}

Status WriteEntities(sqlite3* db, uint64_t block_id, const std::vector<Entity>& entities) {
    if (!db) return Status::InvalidArgument("WriteEntities: null db");
    if (entities.empty()) return Status::Ok();

    sqlite3_stmt* ins = nullptr;
    const char* sql =
        "INSERT INTO entities(block_id, text, type, start_offset, end_offset) "
        "VALUES(?1, ?2, ?3, ?4, ?5)";
    if (sqlite3_prepare_v2(db, sql, -1, &ins, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare insert entities");
    }
    sqlite3_stmt* fts = nullptr;
    const char* fts_sql =
        "INSERT INTO entities_fts(rowid, text, type) VALUES(?1, ?2, ?3)";
    if (sqlite3_prepare_v2(db, fts_sql, -1, &fts, nullptr) != SQLITE_OK) {
        sqlite3_finalize(ins);
        return SqlErr(db, "prepare insert entities_fts");
    }

    Status result = Status::Ok();
    for (const Entity& e : entities) {
        sqlite3_reset(ins);
        sqlite3_bind_int64(ins, 1, block_id);
        sqlite3_bind_text(ins, 2, e.text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, e.type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 4, e.start_offset);
        sqlite3_bind_int(ins, 5, e.end_offset);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            result = SqlErr(db, "insert entity");
            break;
        }
        const int64_t entity_id = sqlite3_last_insert_rowid(db);
        // Keep the external-content FTS5 index in sync (rowid == entity_id).
        sqlite3_reset(fts);
        sqlite3_bind_int64(fts, 1, entity_id);
        sqlite3_bind_text(fts, 2, e.text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(fts, 3, e.type.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(fts) != SQLITE_DONE) {
            result = SqlErr(db, "insert entity_fts");
            break;
        }
    }
    sqlite3_finalize(ins);
    sqlite3_finalize(fts);
    return result;
}

Status WriteEnrichment(sqlite3* db, uint64_t block_id, const EnrichResult& result) {
    if (!db) return Status::InvalidArgument("WriteEnrichment: null db");
    char* err = nullptr;
    if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string d = err ? err : "unknown"; sqlite3_free(err);
        return Status::Internal("CX_ERR_ENRICHER_STORE: begin: " + d);
    }
    Status s = WriteBlockEnrichment(db, block_id, result);
    if (s.ok()) s = WriteEntities(db, block_id, result.entities);
    const char* fin = s.ok() ? "COMMIT" : "ROLLBACK";
    sqlite3_exec(db, fin, nullptr, nullptr, nullptr);
    return s;
}

std::vector<Entity> ReadEntities(sqlite3* db, uint64_t block_id) {
    std::vector<Entity> out;
    if (!db) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT text, type, start_offset, end_offset FROM entities "
        "WHERE block_id=?1 ORDER BY entity_id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(stmt, 1, block_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Entity e;
        const unsigned char* t = sqlite3_column_text(stmt, 0);
        const unsigned char* ty = sqlite3_column_text(stmt, 1);
        e.text = t ? reinterpret_cast<const char*>(t) : "";
        e.type = ty ? reinterpret_cast<const char*>(ty) : "";
        e.start_offset = sqlite3_column_int(stmt, 2);
        e.end_offset = sqlite3_column_int(stmt, 3);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

}  // namespace cortrix::spc
