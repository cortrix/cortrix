#include <cstdint>
#include "cortrix/scoring/scoring_store.h"

#include <string>

namespace cortrix::scoring {

Status WriteScore(sqlite3* db, uint64_t block_id, float semantic_score) {
    if (!db) return Status::InvalidArgument("WriteScore: null db");
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE blocks SET semantic_score=?1 WHERE block_id=?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Status::Internal(std::string("CX_ERR_SCORING_STORE: prepare update: ") +
                                sqlite3_errmsg(db));
    }
    sqlite3_bind_double(stmt, 1, semantic_score);
    // block_id uint64 → int64 is the same bit-preserving cast as id::ToSqliteInt (the
    // value block_insert bound), so the WHERE matches the row just inserted.
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(block_id));
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return Status::Internal(std::string("CX_ERR_SCORING_STORE: update semantic_score: ") +
                                sqlite3_errmsg(db));
    }
    return Status::Ok();
}

}  // namespace cortrix::scoring
