#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

// Opaque SQLite handle (forward-declared C type) — see CortrixStore::db_handle().
struct sqlite3;

namespace cortrix::store {

/// Which of `doc_ids` name a soft-deleted document (status 'deleted') in the
/// per-unit store behind `db`. Retrieval drops the hits of the returned documents,
/// so a soft-deleted document is invisible on every route, as it already is to
/// the list and status endpoints.
///
/// Empty ids and repeats are ignored. An id with no row is not returned: a missing
/// row is not a deleted one, and the callers keep such hits. A null `db` (the
/// lightweight store fakes expose no handle) returns an empty set. A prepare or
/// step failure returns an error, and the callers then leave their hits unfiltered.
/// The ids are bound in chunks of at most 500 parameters, below SQLite's smallest
/// default limit; the cost is bounded by the candidate pool, not by the number of
/// deleted documents.
Result<std::unordered_set<std::string>> LoadDeletedDocIds(
    sqlite3* db, const std::vector<std::string>& doc_ids);

}  // namespace cortrix::store
