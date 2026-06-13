#include "cortrix/store/sqlite_parent_chunk_store.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>
#include <utility>

#include "cortrix/logging/logging.h"
#include "cortrix/store/parent_chunk_schema.h"
#include "cortrix/store/store_errors.h"

namespace cortrix::store {
namespace {

using cortrix::chunker::ChildChunk;
using cortrix::chunker::DocumentMetadata;
using cortrix::chunker::ParentChunk;

// Serialize DocumentMetadata to the JSON shape F06 ParseDocMeta() reads back
// (parser.cpp § ParseDocMeta), so parents.metadata_json round-trips with the F06
// SoT field names. F03/F08 enrich this same blob per-parent (NER/Summary, § 3.2).
std::string MetaToJson(const DocumentMetadata& m) {
    nlohmann::json j;
    j["filename"] = m.filename;
    j["doc_title"] = m.doc_title;
    j["mime_type"] = m.mime_type;
    j["file_size_bytes"] = m.file_size_bytes;
    j["page_count"] = m.page_count;
    j["doc_language"] = m.doc_language;
    j["upload_timestamp"] = m.upload_timestamp;
    j["parse_time_ms"] = m.parse_time_ms;
    return j.dump();
}

DocumentMetadata MetaFromJson(const std::string& s) {
    DocumentMetadata m;
    nlohmann::json j = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return m;
    m.filename = j.value("filename", "");
    m.doc_title = j.value("doc_title", "");
    m.mime_type = j.value("mime_type", "");
    m.file_size_bytes = j.value("file_size_bytes", static_cast<int64_t>(0));
    m.page_count = j.value("page_count", -1);
    m.doc_language = j.value("doc_language", "");
    m.upload_timestamp = j.value("upload_timestamp", static_cast<int64_t>(0));
    m.parse_time_ms = j.value("parse_time_ms", static_cast<int64_t>(0));
    return m;
}

// Bind a possibly-empty TEXT (empty parent_id for flat children → SQL NULL).
void BindTextOrNull(sqlite3_stmt* st, int idx, const std::string& s) {
    if (s.empty()) sqlite3_bind_null(st, idx);
    else sqlite3_bind_text(st, idx, s.c_str(), -1, SQLITE_TRANSIENT);
}

}  // namespace

SqliteParentChunkStore::SqliteParentChunkStore(std::string db_path)
    : db_path_(std::move(db_path)) {}

SqliteParentChunkStore::~SqliteParentChunkStore() { Close(); }

Status SqliteParentChunkStore::Open() {
    std::lock_guard<std::mutex> lock(mu_);
    if (db_) return Status::Ok();
    // FULLMUTEX for defense-in-depth (mirrors CortrixStoreSqlite SEG-001), so a
    // store shared across threads / created-destroyed rapidly in test runners is
    // safe alongside the app-level mu_.
    int rc = sqlite3_open_v2(db_path_.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return StoreStatus(StatusCode::kInternal, store_errors::kDbError,
                           "open '" + db_path_ + "': " + msg);
    }
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);
    Status s = Migrate();
    if (!s.ok()) { sqlite3_close(db_); db_ = nullptr; return s; }
    return Status::Ok();
}

void SqliteParentChunkStore::Close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

Status SqliteParentChunkStore::Migrate() {
    // [A unified-blocks] parents is kept; children is the legacy standalone table
    // (under A, child chunks live in `blocks` — see parent_chunk_schema.h). This
    // standalone store still creates both for its own unit tests until the SPC
    // write path targets blocks (D3.5).
    for (const char* ddl : {kParentsSchemaSql, kChildrenSchemaSql}) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, ddl, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "migration failed";
            sqlite3_free(err);
            CORTRIX_LOG_ERROR("parent_chunk_store", "schema migration failed: {}", msg);
            return StoreStatus(StatusCode::kInternal, store_errors::kDbError, msg);
        }
    }
    return Status::Ok();
}

Status SqliteParentChunkStore::PutParentWithChildren(
    const ParentChunk& parent, const std::vector<ChildChunk>& children) {
    return PutChunkerOutput({parent}, children);
}

Status SqliteParentChunkStore::PutChunkerOutput(
    const std::vector<ParentChunk>& parents, const std::vector<ChildChunk>& children) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return StoreStatus(StatusCode::kInternal, store_errors::kDbError, "store not open");

    auto fail = [&](const std::string& detail) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return StoreStatus(StatusCode::kInternal, store_errors::kDbError, detail);
    };

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return StoreStatus(StatusCode::kInternal, store_errors::kDbError,
                           std::string("BEGIN: ") + sqlite3_errmsg(db_));
    }

    // --- parents ---
    {
        const char* sql =
            "INSERT INTO parents (parent_id,doc_id,namespace_id,parent_text,token_count,"
            "page_start,page_end,byte_offset_start,byte_offset_end,metadata_json,created_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?)";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            return fail(std::string("prepare parents: ") + sqlite3_errmsg(db_));
        }
        for (const auto& p : parents) {
            sqlite3_bind_text(st, 1, p.parent_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, p.doc_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, p.namespace_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, p.parent_text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 5, p.token_count);
            sqlite3_bind_int64(st, 6, p.page_start);
            sqlite3_bind_int64(st, 7, p.page_end);
            sqlite3_bind_int64(st, 8, static_cast<sqlite3_int64>(p.byte_offset_start));
            sqlite3_bind_int64(st, 9, static_cast<sqlite3_int64>(p.byte_offset_end));
            std::string mj = MetaToJson(p.metadata);
            sqlite3_bind_text(st, 10, mj.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 11, p.created_at);
            int rc = sqlite3_step(st);
            sqlite3_reset(st);
            if (rc != SQLITE_DONE) {
                std::string msg = std::string("insert parent '") + p.parent_id + "': " + sqlite3_errmsg(db_);
                sqlite3_finalize(st);
                return fail(msg);
            }
        }
        sqlite3_finalize(st);
    }

    // --- children ---
    {
        const char* sql =
            "INSERT INTO children (child_id,parent_id,doc_id,namespace_id,child_text,"
            "token_count,parent_offset,chunk_index,metadata_json,created_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            return fail(std::string("prepare children: ") + sqlite3_errmsg(db_));
        }
        for (const auto& c : children) {
            sqlite3_bind_text(st, 1, c.child_id.c_str(), -1, SQLITE_TRANSIENT);
            BindTextOrNull(st, 2, c.parent_id);
            sqlite3_bind_text(st, 3, c.doc_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, c.namespace_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 5, c.child_text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 6, c.token_count);
            sqlite3_bind_int64(st, 7, c.parent_offset);
            sqlite3_bind_int64(st, 8, c.chunk_index);
            std::string mj = MetaToJson(c.metadata);
            sqlite3_bind_text(st, 9, mj.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 10, c.created_at);
            int rc = sqlite3_step(st);
            sqlite3_reset(st);
            if (rc != SQLITE_DONE) {
                std::string msg = std::string("insert child '") + c.child_id + "': " + sqlite3_errmsg(db_);
                sqlite3_finalize(st);
                return fail(msg);
            }
        }
        sqlite3_finalize(st);
    }

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail(std::string("COMMIT: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

namespace {
// Read one parents-row from a stepped statement (column order matches the SELECT).
ParentChunk ReadParentRow(sqlite3_stmt* st) {
    ParentChunk p;
    auto col_text = [&](int i) -> std::string {
        const unsigned char* t = sqlite3_column_text(st, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    };
    p.parent_id = col_text(0);
    p.doc_id = col_text(1);
    p.namespace_id = col_text(2);
    p.parent_text = col_text(3);
    p.token_count = static_cast<uint32_t>(sqlite3_column_int64(st, 4));
    p.page_start = static_cast<uint32_t>(sqlite3_column_int64(st, 5));
    p.page_end = static_cast<uint32_t>(sqlite3_column_int64(st, 6));
    p.byte_offset_start = static_cast<uint64_t>(sqlite3_column_int64(st, 7));
    p.byte_offset_end = static_cast<uint64_t>(sqlite3_column_int64(st, 8));
    p.metadata = MetaFromJson(col_text(9));
    p.created_at = sqlite3_column_int64(st, 10);
    return p;
}

constexpr const char* kParentSelectCols =
    "parent_id,doc_id,namespace_id,parent_text,token_count,page_start,page_end,"
    "byte_offset_start,byte_offset_end,metadata_json,created_at";
}  // namespace

Result<ParentChunk> SqliteParentChunkStore::GetParent(const std::string& parent_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return StoreStatus(StatusCode::kInternal, store_errors::kDbError, "store not open");

    std::string sql = std::string("SELECT ") + kParentSelectCols + " FROM parents WHERE parent_id = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return StoreStatus(StatusCode::kInternal, store_errors::kDbError,
                           std::string("prepare GetParent: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(st, 1, parent_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        ParentChunk p = ReadParentRow(st);
        sqlite3_finalize(st);
        return p;
    }
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) {
        return StoreStatus(StatusCode::kNotFound, store_errors::kNotFound,
                           "parent_id '" + parent_id + "' not found");
    }
    return StoreStatus(StatusCode::kInternal, store_errors::kDbError,
                       std::string("step GetParent: ") + sqlite3_errmsg(db_));
}

Result<std::vector<ParentChunk>> SqliteParentChunkStore::BulkGetParents(
    const std::vector<std::string>& parent_ids) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return StoreStatus(StatusCode::kInternal, store_errors::kDbError, "store not open");

    std::vector<ParentChunk> out;
    if (parent_ids.empty()) return out;

    // Prepare once, bind+step per id. Partial hits are fine (missing ids dropped);
    // result order matches input order (§ 2.5 BulkGetParents contract).
    std::string sql = std::string("SELECT ") + kParentSelectCols + " FROM parents WHERE parent_id = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return StoreStatus(StatusCode::kInternal, store_errors::kDbError,
                           std::string("prepare BulkGetParents: ") + sqlite3_errmsg(db_));
    }
    out.reserve(parent_ids.size());
    for (const auto& id : parent_ids) {
        sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            out.push_back(ReadParentRow(st));
        } else if (rc != SQLITE_DONE) {
            std::string msg = std::string("step BulkGetParents '") + id + "': " + sqlite3_errmsg(db_);
            sqlite3_finalize(st);
            return StoreStatus(StatusCode::kInternal, store_errors::kDbError, msg);
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return out;
}

}  // namespace cortrix::store
