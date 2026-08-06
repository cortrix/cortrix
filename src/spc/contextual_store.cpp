#include <cstdint>
#include "cortrix/spc/contextual_store.h"

#include <cstring>
#include <vector>

#include "cortrix/id/hash.h"  // HashChildIdToBlockId (contextual label derivation)

namespace cortrix::spc {

namespace {

Status SqlErr(sqlite3* db, const char* what) {
    return Status::Internal(std::string("CX_ERR_CONTEXTUAL_STORE: ") + what + ": " +
                            (db ? sqlite3_errmsg(db) : "null db"));
}

}  // namespace

Status WriteContextualized(sqlite3* db, uint64_t block_id, const EnrichResult& result) {
    if (!db) return Status::InvalidArgument("WriteContextualized: null db");

    // No contextual output → leave the columns at their DEFAULT (a chain without it / L1
    // default / pre-LLM state). contextualized_status != 0 means the stage ran (1
    // generated / 2 failed / 3 skipped_no_llm); an engaged contextualized_text /
    // embedding optional also signals real output even at status 0 transiently.
    const bool contextual_ran = result.contextualized_status != 0 ||
                         result.contextualized_text.has_value() ||
                         result.contextualized_embedding.has_value();
    if (!contextual_ran) return Status::Ok();

    // Pack the contextualized embedding (and the original dense embedding, dual-vector
    // double-vector coexistence) as contiguous float32 BLOBs. The original dense
    // `embedding` column is the second vector; the contextual stage itself does not own the
    // child's primary embedding (that lives in P-HNSW), so we only write the
    // contextualized_embedding here — the `embedding` column stays NULL unless a
    // later Feature populates it (read path tolerates NULL → dense fallback).
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE blocks SET contextualized_text=?1, contextualized_embedding=?2, "
        "contextualized_status=?3 WHERE block_id=?4";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare update blocks contextualized");
    }

    if (result.contextualized_text.has_value()) {
        sqlite3_bind_text(stmt, 1, result.contextualized_text->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 1);
    }

    std::vector<uint8_t> emb_blob;
    if (result.contextualized_embedding.has_value() &&
        !result.contextualized_embedding->empty()) {
        const auto& v = *result.contextualized_embedding;
        emb_blob.resize(v.size() * sizeof(float));
        std::memcpy(emb_blob.data(), v.data(), emb_blob.size());
        sqlite3_bind_blob(stmt, 2, emb_blob.data(),
                          static_cast<int>(emb_blob.size()), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 2);
    }

    sqlite3_bind_int(stmt, 3, result.contextualized_status);
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(block_id));

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SqlErr(db, "update blocks contextualized");
    return Status::Ok();
}

uint64_t DeriveContextualVecLabel(const std::string& child_id) {
    return cortrix::id::HashChildIdToBlockId(child_id + ":ctx");
}

Status WriteContextualVecLabel(sqlite3* db, const ContextualVecLabelRow& row) {
    if (!db) return Status::InvalidArgument("WriteContextualVecLabel: null db");
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO contextual_vec_labels(label, block_id, child_id)"
        " VALUES(?1, ?2, ?3)"
        " ON CONFLICT(label) DO UPDATE SET"
        " block_id=excluded.block_id, child_id=excluded.child_id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare upsert contextual_vec_labels");
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(row.label));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(row.block_id));
    sqlite3_bind_text(stmt, 3, row.child_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SqlErr(db, "upsert contextual_vec_labels");
    return Status::Ok();
}

Result<ContextualVecLabelRow> GetContextualVecLabel(sqlite3* db, uint64_t label) {
    if (!db) return Status::InvalidArgument("GetContextualVecLabel: null db");
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT label, block_id, child_id FROM contextual_vec_labels WHERE label=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare get contextual_vec_labels");
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(label));
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            return Status::NotFound("CX_ERR_CONTEXTUAL_STORE: label not found");
        }
        return SqlErr(db, "get contextual_vec_labels");
    }
    ContextualVecLabelRow row;
    row.label = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    row.block_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    const unsigned char* child = sqlite3_column_text(stmt, 2);
    row.child_id = child ? reinterpret_cast<const char*>(child) : "";
    sqlite3_finalize(stmt);
    return row;
}

}  // namespace cortrix::spc
