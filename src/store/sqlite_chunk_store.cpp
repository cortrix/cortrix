#include "cortrix/store/sqlite_chunk_store.h"

#include <sqlite3.h>

#include <string>

#include "cortrix/store/store_errors.h"

namespace cortrix::store {
namespace {

// SELECT column order → ChunkRecord field order (child_id, parent_id, content,
// chunk_index). content := blocks.content_text — the chunk full text, written by
// BlockAssembler::AssembleChild as `block.content_text = child.child_text`, so the
// reverse-lookup needs no F09 blob deserialization (A unified blocks, F02 §2.1-bis).
constexpr const char* kChildCols = "child_id, parent_id, content_text, chunk_index";

bool ColumnExists(sqlite3* db, const char* table, const char* column) {
    if (db == nullptr) return false;
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    bool exists = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(st, 1);
        if (name != nullptr && std::string(reinterpret_cast<const char*>(name)) == column) {
            exists = true;
            break;
        }
    }
    sqlite3_finalize(st);
    return exists;
}

std::string SelectCols(bool has_enriched_score, bool has_semantic_score) {
    std::string cols = kChildCols;
    if (has_enriched_score) cols += ", enriched_score";
    if (has_semantic_score) cols += ", semantic_score";
    return cols;
}

ChunkRecord ReadChunkRow(sqlite3_stmt* st, bool has_enriched_score, bool has_semantic_score) {
    ChunkRecord r;
    auto col_text = [&](int i) -> std::string {
        const unsigned char* t = sqlite3_column_text(st, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    };
    r.child_id = col_text(0);
    r.parent_id = col_text(1);
    r.content = col_text(2);
    r.chunk_index = sqlite3_column_int(st, 3);
    int col = 4;
    if (has_enriched_score) {
        if (sqlite3_column_type(st, col) != SQLITE_NULL) {
            r.score_signals.enriched_score =
                static_cast<float>(sqlite3_column_double(st, col));
        }
        ++col;
    }
    if (has_semantic_score) {
        if (sqlite3_column_type(st, col) != SQLITE_NULL) {
            r.score_signals.semantic_score =
                static_cast<float>(sqlite3_column_double(st, col));
        }
        ++col;
    }
    return r;
}

}  // namespace

SqliteChunkStore::SqliteChunkStore(sqlite3* db) : db_(db) {}

void SqliteChunkStore::EnsureScoreColumnCacheLocked() {
    if (score_columns_checked_) return;
    has_enriched_score_ = ColumnExists(db_, "blocks", "enriched_score");
    has_semantic_score_ = ColumnExists(db_, "blocks", "semantic_score");
    score_columns_checked_ = true;
}

Result<ChunkRecord> SqliteChunkStore::Get(const std::string& child_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) {
        return StoreStatus(StatusCode::kInternal, store_errors::kDbError,
                           "chunk store: null db handle");
    }
    EnsureScoreColumnCacheLocked();
    const std::string sql =
        std::string("SELECT ") + SelectCols(has_enriched_score_, has_semantic_score_) +
        " FROM blocks WHERE child_id = ?1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return StoreStatus(StatusCode::kInternal, store_errors::kDbError,
                           std::string("prepare Get: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(st, 1, child_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        ChunkRecord r = ReadChunkRow(st, has_enriched_score_, has_semantic_score_);
        sqlite3_finalize(st);
        return r;
    }
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) {
        return StoreStatus(StatusCode::kNotFound, store_errors::kNotFound,
                           "child_id '" + child_id + "' not found");
    }
    return StoreStatus(StatusCode::kInternal, store_errors::kDbError,
                       std::string("step Get: ") + sqlite3_errmsg(db_));
}

std::vector<ChunkRecord> SqliteChunkStore::GetBatch(
    const std::vector<std::string>& child_ids, std::vector<std::string>* missing_ids) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ChunkRecord> out;
    if (child_ids.empty()) return out;

    auto all_missing = [&]() {
        if (missing_ids) {
            for (const auto& cid : child_ids) missing_ids->push_back(cid);
        }
    };
    if (!db_) { all_missing(); return out; }
    EnsureScoreColumnCacheLocked();

    const std::string sql =
        std::string("SELECT ") + SelectCols(has_enriched_score_, has_semantic_score_) +
        " FROM blocks WHERE child_id = ?1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        all_missing();
        return out;
    }
    out.reserve(child_ids.size());
    for (const auto& cid : child_ids) {
        sqlite3_bind_text(st, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            out.push_back(ReadChunkRow(st, has_enriched_score_, has_semantic_score_));
        } else if (missing_ids) {
            missing_ids->push_back(cid);
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<ChunkRecord> SqliteChunkStore::GetChunksByDocId(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ChunkRecord> out;
    if (!db_) return out;
    EnsureScoreColumnCacheLocked();

    const std::string sql = std::string("SELECT ") +
        SelectCols(has_enriched_score_, has_semantic_score_) +
        " FROM blocks WHERE doc_id = ?1 AND child_id IS NOT NULL ORDER BY chunk_index ASC";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_text(st, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(ReadChunkRow(st, has_enriched_score_, has_semantic_score_));
    }
    sqlite3_finalize(st);
    return out;
}

}  // namespace cortrix::store
