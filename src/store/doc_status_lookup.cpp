#include "cortrix/store/doc_status_lookup.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstddef>

namespace cortrix::store {

namespace {

constexpr std::size_t kMaxIdsPerStatement = 500;

}  // namespace

std::string HiddenDocPredicate(const std::string& alias) {
    const std::string& a = alias;
    return "(+" + a + ".status = 'deleted' OR (+" + a +
           ".source_type IN ('http_upload', 'watch_dir') AND +" + a +
           ".status != 'ready' AND EXISTS (SELECT 1 FROM documents o"
           " WHERE o.source_type = " + a + ".source_type"
           " AND o.source_path = " + a + ".source_path"
           " AND o.doc_id != " + a + ".doc_id AND +o.status != 'deleted'"
           " AND (+o.status = 'ready' OR o.created_at < " + a + ".created_at"
           " OR (o.created_at = " + a + ".created_at AND o.doc_id < " + a + ".doc_id)))))";
}

Result<std::unordered_set<std::string>> LoadHiddenDocIds(
    sqlite3* db, const std::vector<std::string>& doc_ids) {
    std::unordered_set<std::string> hidden;
    if (db == nullptr) return hidden;

    std::vector<const std::string*> ids;
    ids.reserve(doc_ids.size());
    std::unordered_set<std::string> seen;
    for (const auto& id : doc_ids) {
        if (!id.empty() && seen.insert(id).second) ids.push_back(&id);
    }

    const std::string predicate = HiddenDocPredicate("d");
    for (std::size_t start = 0; start < ids.size(); start += kMaxIdsPerStatement) {
        const std::size_t n = std::min(kMaxIdsPerStatement, ids.size() - start);
        std::string sql = "SELECT d.doc_id FROM documents d WHERE d.doc_id IN (";
        for (std::size_t i = 0; i < n; ++i) sql += (i == 0 ? "?" : ",?");
        sql += ") AND " + predicate;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return Status::Internal(std::string("LoadHiddenDocIds prepare: ") +
                                    sqlite3_errmsg(db));
        }
        for (std::size_t i = 0; i < n; ++i) {
            sqlite3_bind_text(stmt, static_cast<int>(i + 1), ids[start + i]->c_str(), -1,
                              SQLITE_TRANSIENT);
        }
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text != nullptr) hidden.insert(reinterpret_cast<const char*>(text));
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            return Status::Internal(std::string("LoadHiddenDocIds step: ") +
                                    sqlite3_errmsg(db));
        }
    }
    return hidden;
}

}  // namespace cortrix::store
