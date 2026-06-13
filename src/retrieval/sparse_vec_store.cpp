#include "cortrix/retrieval/sparse_vec_store.h"

#include <vector>

#include "cortrix/id/types.h"  // id::ToSqliteInt (uint64 block_id → sqlite int64)
#include "cortrix/retrieval/sparse_error.h"

namespace cortrix::retrieval {

Status WriteSparseVec(sqlite3* db, uint64_t block_id, const SparseVector& vec) {
    if (db == nullptr) {
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            "WriteSparseVec: null db handle");
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE blocks SET sparse_vec = ?1 WHERE block_id = ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            std::string("WriteSparseVec prepare: ") + sqlite3_errmsg(db));
    }

    // Empty vector (dead chunk, §6.5) → SQL NULL. A non-empty vector serializes to
    // the §4.2 packed BLOB; an out-of-range term_id fails serialization.
    std::vector<uint8_t> blob;
    bool serialized_ok = true;
    if (!vec.empty()) {
        blob = SerializeSparseVec(vec, &serialized_ok);
        if (!serialized_ok) {
            sqlite3_finalize(stmt);
            return SparseStatus(SparseErrorCode::kSparseSerializeFailed,
                                "WriteSparseVec: serialize failed (term_id out of range)");
        }
    }
    if (blob.empty()) {
        sqlite3_bind_null(stmt, 1);
    } else {
        sqlite3_bind_blob(stmt, 1, blob.data(), static_cast<int>(blob.size()),
                          SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt, 2, cortrix::id::ToSqliteInt(block_id));

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            std::string("WriteSparseVec step: ") + sqlite3_errmsg(db));
    }
    return Status::Ok();
}

}  // namespace cortrix::retrieval
