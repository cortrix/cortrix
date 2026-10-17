#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

// Opaque SQLite handle (forward-declared C type) — see CortrixStore::db_handle().
struct sqlite3;

namespace cortrix::store {

/// The SQL condition, over the `documents` row named `alias`, under which that row's
/// content is hidden from retrieval:
///   - the row is soft-deleted (status 'deleted'); or
///   - it is a version of an uploaded or watched file (http_upload, watch_dir) that is
///     not ready, while another live row of the same source is ready or sorts before
///     it (created_at, then doc_id).
/// So of the live rows of one such source, the ready ones are served when there are
/// any, and otherwise only the oldest: a replacement is not served until it is ready,
/// and a newer failed attempt never beside an older live one. Every other row is
/// served as before.
///
/// The unary plus keeps the planner on the row's primary key (or the full-text join)
/// outside and on idx_doc_source inside the subquery; without it a lookup of more than
/// a few ids becomes a scan of every versioned row. The inner table is aliased, so the
/// condition also binds to an unaliased `documents`.
std::string HiddenDocPredicate(const std::string& alias);

/// Which of `doc_ids` name a row that HiddenDocPredicate hides in the per-unit store
/// behind `db`. Retrieval drops the hits of the returned documents, so such a row is
/// invisible on every route; the list and status endpoints still show it with its own
/// status.
///
/// Empty ids and repeats are ignored. An id with no row is not returned: a missing
/// row is not a hidden one, and the callers keep such hits. A null `db` (the
/// lightweight store fakes expose no handle) returns an empty set. A prepare or
/// step failure returns an error, and the callers then leave their hits unfiltered.
/// The ids are bound in chunks of at most 500 parameters, below SQLite's smallest
/// default limit; each chunk is its own statement. The cost is bounded by the
/// candidate pool, not by the number of hidden documents.
Result<std::unordered_set<std::string>> LoadHiddenDocIds(
    sqlite3* db, const std::vector<std::string>& doc_ids);

}  // namespace cortrix::store
