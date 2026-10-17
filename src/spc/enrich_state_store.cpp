#include "cortrix/spc_enricher/enrich_state_store.h"

#include <string>
#include <unordered_map>

#include "cortrix/logging/logging.h"
#include "cortrix/spc/contextual_store.h"  // DeleteContextualVecLabel
#include "cortrix/spc/index_gates.h"  // the ingest's index gates (shared with the pipeline and the worker)

namespace cortrix::spc {

namespace {

Status SqlErr(sqlite3* db, const char* what) {
    return Status::Internal(std::string("CX_ERR_ENRICH_STATE_STORE: ") + what + ": " +
                            (db ? sqlite3_errmsg(db) : "null db"));
}

EnrichStateRow RowFromStmt(sqlite3_stmt* stmt) {
    EnrichStateRow r;
    r.block_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    const unsigned char* doc = sqlite3_column_text(stmt, 1);
    r.doc_id = doc ? reinterpret_cast<const char*>(doc) : "";
    const unsigned char* child = sqlite3_column_text(stmt, 2);
    r.child_id = child ? reinterpret_cast<const char*>(child) : "";
    const unsigned char* status = sqlite3_column_text(stmt, 3);
    r.status = status ? reinterpret_cast<const char*>(status) : "";
    const unsigned char* members = sqlite3_column_text(stmt, 4);
    r.failed_members = members ? reinterpret_cast<const char*>(members) : "";
    r.attempts = sqlite3_column_int(stmt, 5);
    const unsigned char* err = sqlite3_column_text(stmt, 6);
    r.last_error = err ? reinterpret_cast<const char*>(err) : "";
    r.next_retry_at = sqlite3_column_type(stmt, 7) == SQLITE_NULL
                          ? 0
                          : sqlite3_column_int64(stmt, 7);
    r.updated_at = sqlite3_column_int64(stmt, 8);
    return r;
}

constexpr char kRowColumns[] =
    "block_id, doc_id, child_id, status, failed_members, attempts, last_error, "
    "next_retry_at, updated_at";

}  // namespace

Status UpsertEnrichState(sqlite3* db, const EnrichStateRow& row) {
    if (!db) return Status::InvalidArgument("UpsertEnrichState: null db");
    if (row.status.empty()) {
        return Status::InvalidArgument("UpsertEnrichState: empty status");
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO enrich_state(block_id, doc_id, child_id, status, failed_members,"
        " attempts, last_error, next_retry_at, updated_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
        " ON CONFLICT(block_id) DO UPDATE SET"
        " doc_id=excluded.doc_id, child_id=excluded.child_id, status=excluded.status,"
        " failed_members=excluded.failed_members, attempts=excluded.attempts,"
        " last_error=excluded.last_error, next_retry_at=excluded.next_retry_at,"
        " updated_at=excluded.updated_at";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare upsert enrich_state");
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(row.block_id));
    sqlite3_bind_text(stmt, 2, row.doc_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, row.child_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, row.status.c_str(), -1, SQLITE_TRANSIENT);
    if (row.failed_members.empty()) {
        sqlite3_bind_null(stmt, 5);
    } else {
        sqlite3_bind_text(stmt, 5, row.failed_members.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, 6, row.attempts);
    if (row.last_error.empty()) {
        sqlite3_bind_null(stmt, 7);
    } else {
        sqlite3_bind_text(stmt, 7, row.last_error.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (row.next_retry_at <= 0) {
        sqlite3_bind_null(stmt, 8);
    } else {
        sqlite3_bind_int64(stmt, 8, row.next_retry_at);
    }
    sqlite3_bind_int64(stmt, 9, row.updated_at);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SqlErr(db, "upsert enrich_state");
    return Status::Ok();
}

Result<std::vector<EnrichStateRow>> ListEnrichStateForDoc(
    sqlite3* db, const std::string& doc_id, const std::string& status_filter) {
    if (!db) return Status::InvalidArgument("ListEnrichStateForDoc: null db");
    const std::string sql =
        std::string("SELECT ") + kRowColumns +
        " FROM enrich_state WHERE doc_id=?1" +
        (status_filter.empty() ? "" : " AND status=?2") + " ORDER BY block_id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare list enrich_state for doc");
    }
    sqlite3_bind_text(stmt, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    if (!status_filter.empty()) {
        sqlite3_bind_text(stmt, 2, status_filter.c_str(), -1, SQLITE_TRANSIENT);
    }
    std::vector<EnrichStateRow> rows;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) rows.push_back(RowFromStmt(stmt));
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SqlErr(db, "list enrich_state for doc");
    return rows;
}

Result<std::vector<std::string>> ListDueDocs(sqlite3* db, int64_t now_unix, int limit) {
    if (!db) return Status::InvalidArgument("ListDueDocs: null db");
    if (limit <= 0) return std::vector<std::string>{};
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT doc_id, MIN(next_retry_at) AS due FROM enrich_state"
        " WHERE status=?1 AND next_retry_at IS NOT NULL AND next_retry_at<=?2"
        " GROUP BY doc_id ORDER BY due LIMIT ?3";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare list due docs");
    }
    sqlite3_bind_text(stmt, 1, kEnrichStatusPendingRetry, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now_unix);
    sqlite3_bind_int(stmt, 3, limit);
    std::vector<std::string> docs;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char* doc = sqlite3_column_text(stmt, 0);
        docs.emplace_back(doc ? reinterpret_cast<const char*>(doc) : "");
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SqlErr(db, "list due docs");
    return docs;
}

Status LeaseDocRetries(sqlite3* db, const std::string& doc_id, int64_t lease_until,
                       int64_t now_unix) {
    if (!db) return Status::InvalidArgument("LeaseDocRetries: null db");
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE enrich_state SET next_retry_at=?1, updated_at=?2"
        " WHERE doc_id=?3 AND status=?4";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare lease doc retries");
    }
    sqlite3_bind_int64(stmt, 1, lease_until);
    sqlite3_bind_int64(stmt, 2, now_unix);
    sqlite3_bind_text(stmt, 3, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, kEnrichStatusPendingRetry, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SqlErr(db, "lease doc retries");
    return Status::Ok();
}

namespace {

// Each document's processing level, keyed by doc_id — the first half of the
// ingest's index gate (index_gates.h), read the same way by the audit and the
// reconciliation. A document without a row reads as the full level (the caller's
// lookup default); a store without the table (isolated fixtures) yields an empty
// map, logged so a production store in that state is visible rather than silent;
// a scan that breaks off is an error, because leaving some documents at the
// default would misjudge them.
Result<std::unordered_map<std::string, int>> LoadDocumentLevels(sqlite3* db) {
    std::unordered_map<std::string, int> doc_levels;
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT doc_id, processing_level FROM documents";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        CORTRIX_LOG_DEBUG("spc",
            "enrich audit: documents table unavailable, every document read as "
            "full processing level: {}", sqlite3_errmsg(db));
        return doc_levels;
    }
    int drc = SQLITE_OK;
    while ((drc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char* d = sqlite3_column_text(st, 0);
        if (!d) continue;
        const int level = sqlite3_column_type(st, 1) == SQLITE_NULL
                              ? 3
                              : sqlite3_column_int(st, 1);
        doc_levels[reinterpret_cast<const char*>(d)] = level;
    }
    sqlite3_finalize(st);
    if (drc != SQLITE_DONE) return SqlErr(db, "audit documents scan");
    return doc_levels;
}

int DocLevelOf(const std::unordered_map<std::string, int>& levels, const std::string& doc_id) {
    const auto it = levels.find(doc_id);
    return it == levels.end() ? 3 : it->second;
}

}  // namespace

ReconcileOutcome ReconcileExcludedContextualLabels(
    sqlite3* db, const std::function<Status(uint64_t label)>& mark_point_deleted) {
    ReconcileOutcome out;
    if (!db) {
        out.status = Status::InvalidArgument("ReconcileExcludedContextualLabels: null db");
        return out;
    }
    if (!mark_point_deleted) {
        out.status = Status::InvalidArgument("ReconcileExcludedContextualLabels: null callback");
        return out;
    }
    auto levels = LoadDocumentLevels(db);
    if (!levels.ok()) {
        out.status = levels.status();
        return out;
    }

    // Collect first, act afterwards: no DELETE while the SELECT is stepping. The
    // mark is read with the same SQL predicate the audit's pass 3 uses; the join
    // adds only label/block_id/child_id columns, so metadata_json is unambiguous.
    struct Excluded { uint64_t label; };
    std::vector<Excluded> excluded;
    {
        sqlite3_stmt* st = nullptr;
        const std::string sql =
            std::string("SELECT l.label, b.doc_id, ") + kBlockSkipsIndexSql +
            " FROM contextual_vec_labels l JOIN blocks b ON b.block_id = l.block_id"
            " ORDER BY l.label";
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            // No labels table (isolated pre-V2 store or fixture): nothing to remove.
            return out;
        }
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const uint64_t label = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
            const unsigned char* did = sqlite3_column_text(st, 1);
            const std::string doc_id = did ? reinterpret_cast<const char*>(did) : "";
            const bool skip_index = sqlite3_column_int(st, 2) != 0;
            if (!IndexPointsExpected(DocLevelOf(levels.value(), doc_id), skip_index)) {
                excluded.push_back({label});
            }
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            out.status = SqlErr(db, "reconcile labels scan");
            return out;
        }
    }

    for (const Excluded& e : excluded) {
        Status mark = mark_point_deleted(e.label);
        if (!mark.ok()) {  // the row stays; the next audit retries it
            out.status = mark;
            return out;
        }
        Result<bool> del = DeleteContextualVecLabel(db, e.label);
        if (!del.ok()) {
            out.status = del.status();
            return out;
        }
        // Only a row this call deleted counts: another audit that removed it first
        // reports it itself, so concurrent audits never report the same row twice.
        if (del.value()) ++out.removed;
    }
    return out;
}

Result<int> SynthesizeEnrichAuditRows(
    sqlite3* db, const std::unordered_set<std::string>& configured_members,
    int64_t now_unix) {
    if (!db) return Status::InvalidArgument("SynthesizeEnrichAuditRows: null db");

    // Pass 1: child_ids that already have hype blocks (f38 done set).
    std::unordered_set<std::string> hype_sources;
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT DISTINCT json_extract(metadata_json, '$.source_child_id')"
            " FROM blocks WHERE block_type=16 AND metadata_json IS NOT NULL";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            return SqlErr(db, "prepare audit hype sources");
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* c = sqlite3_column_text(st, 0);
            if (c) hype_sources.insert(reinterpret_cast<const char*>(c));
        }
        sqlite3_finalize(st);
    }

    // Pass 1.5 (P7): block_ids whose contextual dual-vector LABEL row exists.
    // f35 "done" requires BOTH the columns (ctx_status==1) AND the ANN label —
    // a store that wrote the columns but never indexed the vector (pre-V2 data,
    // or a partially-lost index) must be re-owed or the contextualized path
    // never gets its votes. Defensive: when the table is absent (isolated
    // pre-V2 store), skip the label check and keep the legacy ctx_status-only
    // semantics rather than re-owing everything.
    std::unordered_set<uint64_t> ctx_labels;
    bool ctx_labels_available = false;
    {
        sqlite3_stmt* st = nullptr;
        const char* sql = "SELECT block_id FROM contextual_vec_labels";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
            ctx_labels_available = true;
            while (sqlite3_step(st) == SQLITE_ROW) {
                ctx_labels.insert(static_cast<uint64_t>(sqlite3_column_int64(st, 0)));
            }
            sqlite3_finalize(st);
        }
    }

    // Pass 1.7: each document's processing level — the first half of the ingest's
    // index gate (index_gates.h). Below the full level the ingest writes neither
    // the contextual index point nor hype blocks, so neither may be owed.
    auto levels = LoadDocumentLevels(db);
    if (!levels.ok()) return levels.status();
    const std::unordered_map<std::string, int>& doc_levels = levels.value();

    // Pass 2: rows the audit must NOT re-synthesize. Two cases: in-flight
    // pending_retry (keep their attempts/backoff) AND failed_permanent (P4: a
    // terminal give-up must stay terminal — re-synthesizing via INSERT OR REPLACE
    // would reset attempts=0 and resurrect a chunk that already hit the 8x/48h
    // ceiling, spinning it forever).
    std::unordered_set<uint64_t> audit_skip;
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT block_id FROM enrich_state WHERE status=?1 OR status=?2";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            return SqlErr(db, "prepare audit tracked");
        }
        sqlite3_bind_text(st, 1, kEnrichStatusPendingRetry, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, kEnrichStatusFailedPermanent, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW) {
            audit_skip.insert(static_cast<uint64_t>(sqlite3_column_int64(st, 0)));
        }
        sqlite3_finalize(st);
    }

    // Pass 3: child rows vs configured artifacts → synthesize. The last column is
    // the cleaner's skip-index mark — the second half of the ingest's index gate;
    // it can be NULL (a NULL or malformed metadata_json), which sqlite3_column_int
    // reads as 0.
    sqlite3_stmt* st = nullptr;
    const std::string sql =
        std::string("SELECT block_id, child_id, doc_id, enriched_score, contextualized_status, ") +
        kBlockSkipsIndexSql +
        " FROM blocks WHERE child_id IS NOT NULL AND child_id != ''";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare audit children");
    }
    int synthesized = 0;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint64_t block_id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        const unsigned char* cid = sqlite3_column_text(st, 1);
        const unsigned char* did = sqlite3_column_text(st, 2);
        const bool has_score = sqlite3_column_type(st, 3) != SQLITE_NULL;
        const int ctx_status = sqlite3_column_type(st, 4) == SQLITE_NULL
                                   ? 0
                                   : sqlite3_column_int(st, 4);
        const bool skip_index = sqlite3_column_int(st, 5) != 0;
        const std::string child_id = cid ? reinterpret_cast<const char*>(cid) : "";
        const std::string doc_id = did ? reinterpret_cast<const char*>(did) : "";
        // The ingest's index gate for this chunk: the document's level (a missing
        // row reads as the full level) and the cleaner's mark. What the gate
        // excluded was never written and is not missing.
        const int doc_level = DocLevelOf(doc_levels, doc_id);
        const bool label_expected = IndexPointsExpected(doc_level, skip_index);
        const bool hype_expected = HypeBlocksExpected(doc_level);
        std::string owed;
        auto owe = [&owed](const char* tok) {
            if (!owed.empty()) owed += ",";
            owed += tok;
        };
        if (configured_members.count("f03") && !has_score) owe("f03");
        // f35 done ⇔ columns written AND, where the ingest indexes the chunk at
        // all, the ANN label indexed (P7): columns-only data on an indexed chunk
        // (pre-V2 / partial loss) is invisible to the contextualized path, while a
        // chunk the gate excluded has no label to miss.
        const bool f35_done =
            ctx_status == 1 &&
            (!label_expected || !ctx_labels_available || ctx_labels.count(block_id) > 0);
        if (configured_members.count("f35") && !f35_done) owe("f35");
        // f38 done ⇔ a hype block names the child — owed only where the ingest
        // writes hype blocks at all (the full processing level).
        if (configured_members.count("f38") && hype_expected &&
            hype_sources.find(child_id) == hype_sources.end()) {
            owe("f38");
        }
        if (owed.empty()) continue;
        if (audit_skip.count(block_id)) continue;
        EnrichStateRow row;
        row.block_id = block_id;
        row.doc_id = doc_id;
        row.child_id = child_id;
        row.status = kEnrichStatusPendingRetry;
        row.failed_members = std::move(owed);
        row.attempts = 0;
        row.last_error = "audit: artifacts missing";
        row.next_retry_at = now_unix;
        row.updated_at = now_unix;
        if (UpsertEnrichState(db, row).ok()) ++synthesized;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return SqlErr(db, "audit children scan");
    return synthesized;
}

Result<EnrichStateCounts> CountEnrichStates(sqlite3* db) {
    if (!db) return Status::InvalidArgument("CountEnrichStates: null db");
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT status, COUNT(*) FROM enrich_state GROUP BY status";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return SqlErr(db, "prepare count enrich_state");
    }
    EnrichStateCounts c;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char* status = sqlite3_column_text(stmt, 0);
        const std::string s = status ? reinterpret_cast<const char*>(status) : "";
        const int64_t n = sqlite3_column_int64(stmt, 1);
        c.total += n;
        if (s == kEnrichStatusOk) c.ok += n;
        else if (s == kEnrichStatusPendingRetry) c.pending_retry += n;
        else if (s == kEnrichStatusFailedPermanent) c.failed_permanent += n;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SqlErr(db, "count enrich_state");
    return c;
}

}  // namespace cortrix::spc
