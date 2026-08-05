#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

struct sqlite3;

namespace cortrix::agent_trace {

/// One citation-provenance row in a GET /interactions/{id}/sources response
///. snippet is already truncated (<=500, first 400 + last 100). The
/// CE shape carries NO highlight ranges (Ent writes those to
/// interaction_sources_extension).
struct InteractionSource {
    std::string source_block_id;
    std::string source_type;          ///< "block" / "memory" / "metadata"
    double relevance_score = 0.0;
    std::string snippet;
};

/// GET /interactions/{id}/sources response. deleted_sources_count is the
/// number of provenance rows whose source_block_id no longer resolves in `blocks`
/// (topic 6 -- historical chunk deleted handling); those rows are omitted from
/// `sources` but counted here so the Agent knows the answer cited now-gone data.
struct InteractionSourcesView {
    std::string interaction_id;
    int source_count = 0;             ///< sources returned (live)
    int deleted_sources_count = 0;
    std::vector<InteractionSource> sources;
};

/// The caller's permission context for an interactions read. The
/// real values come from the AuthContext at the request boundary;
/// standalone tests pass them directly. A handler enforces:
///   - non-admin may only read interactions whose interaction_log.user_id ==
///     requester_user_id (else CX_ERR_TRACE_UNAUTHORIZED, anti-leak);
///   - admin may read across users; a missing interaction is
///     CX_ERR_TRACE_INTERACTION_NOT_FOUND.
struct RequesterContext {
    std::string requester_user_id;
    bool is_admin = false;
};

/// One row in a GET /interactions list response. Projected from the
/// real frozen interaction_log (MVP role/content model) — NOT the §4.2 draft shape
/// (query_text/response_summary), which predates the memory rebuild. query_text is
/// returned in full (§8.3 — not truncated); namespace_id is the real
/// namespace_name column.
struct InteractionListItem {
    std::string interaction_id;       ///< interaction_log.id (UUID)
    std::string session_id;
    std::string user_id;
    std::string namespace_id;         ///< == real interaction_log.namespace_name
    std::string role;                 ///< "user" / "assistant" / "system"
    std::string query_text;           ///< == real interaction_log.content (full)
    std::string query_type;           ///< "semantic" / "sql" / "hybrid" / "chat"
    std::string created_at;           ///< ISO-8601 TEXT
};

/// Filter + pagination for GET /interactions. Defaults match §8.3:
/// limit 50 / cap 200, offset 0, DESC. user_id is admin-only cross-user (a
/// non-admin gets their own rows regardless of this field).
struct InteractionListFilter {
    std::optional<std::string> user_id;       ///< admin cross-user target
    std::optional<std::string> session_id;
    std::optional<std::string> namespace_id;  ///< filters on namespace_name
    std::optional<int64_t> from_timestamp;    ///< Unix ms (compared against ISO created_at)
    std::optional<int64_t> to_timestamp;
    int limit = 50;                           ///< default 50 / cap 200
    int offset = 0;
    std::string sort_order = "DESC";          ///< "ASC" | "DESC"
};

/// Paginated GET /interactions response.
struct InteractionListView {
    std::vector<InteractionListItem> interactions;
    int64_t total_count = 0;
    bool has_next = false;
    int next_offset = 0;
};

/// CE business logic for the citation-provenance + interactions-list endpoints
/// Reads interaction_log (MVP frozen, role/content
/// model) + interaction_sources (CE, this Feature) + probes `blocks` for deleted
/// sources. Standalone: pure DB logic over a borrowed handle; the HTTP route
/// registration (httplib) is D3.5. Thread-safe via a single mutex over the handle.
class InteractionsHandler {
public:
    /// @param db  borrowed handle to the DB holding interaction_log +
    ///            interaction_sources + blocks (the memory/global DB). Not owned.
    explicit InteractionsHandler(sqlite3* db);

    /// GET /interactions/{id}/sources (§8.2, T-106). Permission-checks against
    /// interaction_log.user_id, then returns the live sources (deleted ones
    /// counted in deleted_sources_count). Errors: INTERACTION_NOT_FOUND /
    /// UNAUTHORIZED / INTERNAL (carried as a Status with the CX_ERR_TRACE_* token).
    Result<InteractionSourcesView> GetSources(const std::string& interaction_id,
                                              const RequesterContext& ctx);

    /// GET /interactions (§8.3 — Agent self-service query history core). Lists
    /// interactions with filter + pagination, projected from the real frozen
    /// interaction_log. Permission (§8.3, mirrors §8.2): a non-admin is always
    /// scoped to their own user_id (any user_id filter naming someone else ->
    /// CX_ERR_TRACE_UNAUTHORIZED); an admin may target another user_id (and the
    /// access is logged via the §12 forensics line). admin cross-user with no rows
    /// -> empty list (per §8.3, not an error). Invalid filter -> CX_ERR_TRACE_INVALID_FILTER.
    Result<InteractionListView> ListInteractions(const InteractionListFilter& filter,
                                                 const RequesterContext& ctx);

    /// Truncate a snippet to <=500 chars keeping the first 400 + last 100 with a
    /// [...] marker (§6/§8.2). Exposed for tests.
    static std::string TruncateSnippet(const std::string& snippet);

private:
    sqlite3* db_;                     ///< borrowed (not owned)
    mutable std::mutex mu_;
};

}  // namespace cortrix::agent_trace
