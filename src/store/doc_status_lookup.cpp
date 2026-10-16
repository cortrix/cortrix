#include "cortrix/store/doc_status_lookup.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstddef>

namespace cortrix::store {

namespace {

constexpr std::size_t kMaxIdsPerStatement = 500;

}  // namespace

Result<std::unordered_set<std::string>> LoadDeletedDocIds(
    sqlite3* db, const std::vector<std::string>& doc_ids) {
    std::unordered_set<std::string> deleted;
    if (db == nullptr) return deleted;

    std::vector<const std::string*> ids;
    ids.reserve(doc_ids.size());
    std::unordered_set<std::string> seen;
    for (const auto& id : doc_ids) {
        if (!id.empty() && seen.insert(id).second) ids.push_back(&id);
    }

    for (std::size_t start = 0; start < ids.size(); start += kMaxIdsPerStatement) {
        const std::size_t n = std::min(kMaxIdsPerStatement, ids.size() - start);
        std::string sql =
            "SELECT doc_id FROM documents WHERE status = 'deleted' AND doc_id IN (";
        for (std::size_t i = 0; i < n; ++i) sql += (i == 0 ? "?" : ",?");
        sql += ")";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return Status::Internal(std::string("LoadDeletedDocIds prepare: ") +
                                    sqlite3_errmsg(db));
        }
        for (std::size_t i = 0; i < n; ++i) {
            sqlite3_bind_text(stmt, static_cast<int>(i + 1), ids[start + i]->c_str(), -1,
                              SQLITE_TRANSIENT);
        }
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text != nullptr) deleted.insert(reinterpret_cast<const char*>(text));
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            return Status::Internal(std::string("LoadDeletedDocIds step: ") +
                                    sqlite3_errmsg(db));
        }
    }
    return deleted;
}

}  // namespace cortrix::store
