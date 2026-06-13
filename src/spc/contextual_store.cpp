#include "cortrix/spc/contextual_store.h"

#include <cstring>
#include <vector>

namespace cortrix::spc {

namespace {

Status SqlErr(sqlite3* db, const char* what) {
    return Status::Internal(std::string("CX_ERR_CONTEXTUAL_STORE: ") + what + ": " +
                            (db ? sqlite3_errmsg(db) : "null db"));
}

}  // namespace

Status WriteContextualized(sqlite3* db, uint64_t block_id, const EnrichResult& result) {
    if (!db) return Status::InvalidArgument("WriteContextualized: null db");

    // No F35 output → leave the columns at their DEFAULT (a non-F35 chain / L1
    // default / pre-LLM state). contextualized_status != 0 means F35 ran (1
    // generated / 2 failed / 3 skipped_no_llm); an engaged contextualized_text /
    // embedding optional also signals real output even at status 0 transiently.
    const bool f35_ran = result.contextualized_status != 0 ||
                         result.contextualized_text.has_value() ||
                         result.contextualized_embedding.has_value();
    if (!f35_ran) return Status::Ok();

    // Pack the contextualized embedding (and the original dense embedding, F35-9
    // double-vector coexistence) as contiguous float32 BLOBs. The original dense
    // `embedding` column is the F35-9 second vector; F35 itself does not own the
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

}  // namespace cortrix::spc
