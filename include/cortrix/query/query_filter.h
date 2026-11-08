#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/common/status.h"

namespace cortrix::query {

/// The `filter` object of POST /api/v1/query (`QueryFilter` in api/components/schemas.yaml),
/// parsed once per request and applied to every candidate document in every namespace.
///
///   tags:       array of strings. A document matches when its upload `metadata.tags`
///               (a string or an array of strings) contains at least one of them.
///   date_range: {start?, end?}, each `YYYY-MM-DD`. A document matches when the UTC date
///               of its `created_at` is within [start, end]; a missing bound is open.
///
/// Keys outside the contract are not applied; they are listed in `ignored_keys` so the
/// response can say so instead of silently returning unfiltered results.
struct QueryFilterSpec {
    std::vector<std::string> tags;
    std::optional<std::string> date_start;  ///< inclusive, YYYY-MM-DD
    std::optional<std::string> date_end;    ///< inclusive, YYYY-MM-DD
    std::vector<std::string> ignored_keys;

    /// True when tags or date_range constrain the result set.
    bool active() const { return !tags.empty() || date_start || date_end; }
};

/// Parse `filter` (absent or null → empty spec). A wrong type or date format returns
/// InvalidArgument naming the field; an unknown key is recorded in ignored_keys.
Status ParseQueryFilter(const nlohmann::json& filter, QueryFilterSpec* out);

/// Does a document with this `created_at` (stored as ISO 8601 UTC) and upload metadata
/// JSON pass the filter? An inactive spec matches everything.
bool DocumentMatchesFilter(const QueryFilterSpec& spec, const std::string& created_at,
                           const std::string& metadata_json);

}  // namespace cortrix::query
