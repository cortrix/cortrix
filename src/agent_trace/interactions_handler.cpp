#include <cstdint>
#include "cortrix/agent_trace/interactions_handler.h"

#include <sqlite3.h>

#include <cstdio>
#include <ctime>
#include <vector>

#include "cortrix/agent_trace/agent_trace_error.h"
#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/observability_audit_log.h"

namespace cortrix::agent_trace {

namespace {

constexpr size_t kSnippetMaxBytes = 500;
constexpr size_t kSnippetHeadBytes = 400;  // -- first 400
constexpr size_t kSnippetTailBytes = 100;  // + last 100
constexpr const char* kSnippetMid = "[...]";

std::string ColText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? t : "";
}

// Format Unix ms as the ISO-8601 UTC string the interaction_log.created_at column
// uses, so a time-range filter compares lexically against it (ISO-8601 UTC sorts
// lexicographically). Mirrors AgentTraceCleanupRegistrar::FormatIso8601Utc; kept local to
// avoid coupling the handler to the cleanup module.
std::string ToIso8601Utc(int64_t unix_ms) {
    const std::time_t secs = static_cast<std::time_t>(unix_ms / 1000);
    const int millis = static_cast<int>(unix_ms % 1000);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, millis);
    return std::string(buf);
}

// True iff a `blocks(block_id)` row exists for `block_id`. If the blocks table is
// absent (isolated test DB without the core schema), treat the source as live --
// the deleted-source detection only applies where blocks exists (production).
bool BlockExists(sqlite3* db, const std::string& block_id, bool* blocks_table_present) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM blocks WHERE block_id = ? LIMIT 1",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        *blocks_table_present = false;
        return true;  // no blocks table -> can't tell -> assume live
    }
    *blocks_table_present = true;
    sqlite3_bind_text(stmt, 1, block_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

}  // namespace

std::string InteractionsHandler::TruncateSnippet(const std::string& snippet) {
    if (snippet.size() <= kSnippetMaxBytes) return snippet;
    return snippet.substr(0, kSnippetHeadBytes) + kSnippetMid +
           snippet.substr(snippet.size() - kSnippetTailBytes);
}

InteractionsHandler::InteractionsHandler(sqlite3* db) : db_(db) {}

Result<InteractionSourcesView> InteractionsHandler::GetSources(
    const std::string& interaction_id, const RequesterContext& ctx) {
    std::lock_guard<std::mutex> lock(mu_);

    // 1. Look up the interaction's owner (interaction_log.user_id) for the
    //    permission check + existence. The MVP interaction_log PK is `id` (TEXT).
    std::optional<std::string> owner_user_id;
    bool found = false;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT user_id FROM interaction_log WHERE id = ? LIMIT 1",
                -1, &stmt, nullptr) != SQLITE_OK) {
            return AgentTraceStatus(AgentTraceErrorCode::kInternal, sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(stmt, 1, interaction_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            found = true;
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) owner_user_id = ColText(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (!found) {
        return AgentTraceStatus(AgentTraceErrorCode::kInteractionNotFound,
                         "no interaction with id " + interaction_id);
    }

    // 2. Permission (anti-leak): non-admin may only read their own.
    if (!ctx.is_admin && owner_user_id.value_or("") != ctx.requester_user_id) {
        return AgentTraceStatus(AgentTraceErrorCode::kUnauthorized,
                         "cross-user interaction access requires admin");
    }
    if (ctx.is_admin && owner_user_id.has_value() &&
        *owner_user_id != ctx.requester_user_id) {
        // forensics: admin reading another user's interaction sources.
        ObservabilityAuditLog::Emit(ctx.requester_user_id, *owner_user_id,
                                    ObservabilityAuditLog::Endpoint::kInteractionsSources);
    }

    // 3. Sources: split live vs deleted (source_block_id no longer in blocks).
    InteractionSourcesView out;
    out.interaction_id = interaction_id;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT source_block_id, source_type, relevance_score, snippet "
                "FROM interaction_sources WHERE interaction_id = ? "
                "ORDER BY relevance_score DESC, id ASC",
                -1, &stmt, nullptr) != SQLITE_OK) {
            return AgentTraceStatus(AgentTraceErrorCode::kInternal, sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(stmt, 1, interaction_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string block_id = ColText(stmt, 0);
            bool blocks_present = false;
            if (!BlockExists(db_, block_id, &blocks_present)) {
                out.deleted_sources_count++;
                continue;  // omit deleted sources from the list (counted above)
            }
            InteractionSource s;
            s.source_block_id = block_id;
            s.source_type = ColText(stmt, 1);
            s.relevance_score = sqlite3_column_double(stmt, 2);
            s.snippet = TruncateSnippet(ColText(stmt, 3));
            out.sources.push_back(std::move(s));
        }
        sqlite3_finalize(stmt);
    }
    out.source_count = static_cast<int>(out.sources.size());

    // OBS_SPEC -- count the read by role + endpoint.
    AgentTraceMetrics::Instance().RecordTracesQuery(
        ctx.is_admin ? AgentTraceMetrics::Role::kAdmin : AgentTraceMetrics::Role::kUser,
        AgentTraceMetrics::Endpoint::kInteractionsSources);
    return out;
}

Result<InteractionListView> InteractionsHandler::ListInteractions(
    const InteractionListFilter& filter, const RequesterContext& ctx) {
    // ---- validate (> CX_ERR_TRACE_INVALID_FILTER) ----
    if (filter.limit < 1 || filter.limit > 200) {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter,
                         "limit must be in [1,200], got " + std::to_string(filter.limit));
    }
    if (filter.offset < 0) {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter,
                         "offset must be >= 0, got " + std::to_string(filter.offset));
    }
    if (filter.sort_order != "ASC" && filter.sort_order != "DESC") {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter,
                         "sort_order must be ASC|DESC, got " + filter.sort_order);
    }
    if (filter.from_timestamp.has_value() && filter.to_timestamp.has_value() &&
        *filter.from_timestamp > *filter.to_timestamp) {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter, "from_timestamp > to_timestamp");
    }

    // ---- resolve the effective user scope (permission) ----
    // A non-admin is hard-scoped to their own user_id; a user_id filter naming
    // anyone else is a cross-user attempt -> UNAUTHORIZED (anti-leak). An admin may
    // target a specific user_id (logged) or list across all when unset.
    std::optional<std::string> scope_user_id;
    if (!ctx.is_admin) {
        if (filter.user_id.has_value() && *filter.user_id != ctx.requester_user_id) {
            return AgentTraceStatus(AgentTraceErrorCode::kUnauthorized,
                             "cross-user interaction listing requires admin");
        }
        scope_user_id = ctx.requester_user_id;  // force own rows
    } else {
        scope_user_id = filter.user_id;  // admin: optional target (nullopt = all users)
        if (scope_user_id.has_value() && *scope_user_id != ctx.requester_user_id) {
            ObservabilityAuditLog::Emit(ctx.requester_user_id, *scope_user_id,
                                        ObservabilityAuditLog::Endpoint::kInteractions);
        }
    }

    std::lock_guard<std::mutex> lock(mu_);

    // ---- WHERE + ordered binds (text predicates first, then int/iso time) ----
    std::string where = " WHERE 1=1";
    std::vector<std::string> text_binds;
    auto add_text = [&](const std::string& clause, const std::string& v) {
        where += clause;
        text_binds.push_back(v);
    };
    if (scope_user_id.has_value())        add_text(" AND user_id = ?", *scope_user_id);
    if (filter.session_id.has_value())    add_text(" AND session_id = ?", *filter.session_id);
    if (filter.namespace_id.has_value())  add_text(" AND namespace_name = ?", *filter.namespace_id);
    if (filter.from_timestamp.has_value()) add_text(" AND created_at >= ?",
                                                    ToIso8601Utc(*filter.from_timestamp));
    if (filter.to_timestamp.has_value())   add_text(" AND created_at <= ?",
                                                    ToIso8601Utc(*filter.to_timestamp));

    auto bind_all = [&](sqlite3_stmt* stmt) -> int {
        int idx = 1;
        for (const auto& t : text_binds)
            sqlite3_bind_text(stmt, idx++, t.c_str(), -1, SQLITE_TRANSIENT);
        return idx;
    };

    InteractionListView out;
    // total_count for pagination.
    {
        const std::string sql = "SELECT COUNT(*) FROM interaction_log" + where;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return AgentTraceStatus(AgentTraceErrorCode::kInternal, sqlite3_errmsg(db_));
        }
        bind_all(stmt);
        if (sqlite3_step(stmt) == SQLITE_ROW) out.total_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (filter.offset > 0 && filter.offset >= out.total_count) {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter,
                         "offset " + std::to_string(filter.offset) +
                             " >= total_count " + std::to_string(out.total_count));
    }

    // page query — project the real frozen interaction_log columns.
    {
        const std::string sql =
            "SELECT id, session_id, user_id, namespace_name, role, content, "
            "query_type, created_at FROM interaction_log" + where +
            " ORDER BY created_at " + filter.sort_order + ", id " + filter.sort_order +
            " LIMIT ? OFFSET ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return AgentTraceStatus(AgentTraceErrorCode::kInternal, sqlite3_errmsg(db_));
        }
        int idx = bind_all(stmt);
        sqlite3_bind_int(stmt, idx++, filter.limit);
        sqlite3_bind_int(stmt, idx++, filter.offset);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            InteractionListItem it;
            it.interaction_id = ColText(stmt, 0);
            it.session_id     = ColText(stmt, 1);
            it.user_id        = ColText(stmt, 2);
            it.namespace_id   = ColText(stmt, 3);
            it.role           = ColText(stmt, 4);
            it.query_text     = ColText(stmt, 5);   // full text, not truncated
            it.query_type     = ColText(stmt, 6);
            it.created_at     = ColText(stmt, 7);
            out.interactions.push_back(std::move(it));
        }
        sqlite3_finalize(stmt);
    }

    out.next_offset = filter.offset + static_cast<int>(out.interactions.size());
    out.has_next = out.next_offset < out.total_count;

    AgentTraceMetrics::Instance().RecordTracesQuery(
        ctx.is_admin ? AgentTraceMetrics::Role::kAdmin : AgentTraceMetrics::Role::kUser,
        AgentTraceMetrics::Endpoint::kInteractions);
    return out;
}

}  // namespace cortrix::agent_trace
