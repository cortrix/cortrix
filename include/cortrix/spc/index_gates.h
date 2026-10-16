#pragma once
#include <string>

namespace cortrix::spc {

/// The ingest's rule for what a chunk may put into the vector index, kept in one
/// place for its three consumers: the pipeline applies it when it writes; the
/// enrichment audit must not report as missing what the rule excluded; the
/// backfill worker must not add what the rule excluded. `doc_level` is the
/// document's processing level (the task's level, persisted on the documents row
/// by every shipped producer), `skip_index` the cleaner's mark on the chunk.

/// A chunk's vector points — its primary point and its contextual (dual-vector)
/// point with the label row that resolves it — exist only at the full processing
/// level and only for chunks the cleaner did not mark skip-index.
inline bool IndexPointsExpected(int doc_level, bool skip_index) {
    return doc_level >= 3 && !skip_index;
}

/// Hype question blocks carry a vector, so they exist only at the full processing
/// level. The cleaner's mark does not govern them: the ingest writes them for
/// skip-index chunks too.
inline bool HypeBlocksExpected(int doc_level) { return doc_level >= 3; }

/// The cleaner's mark as stored on a block: `"cleaning.skip_index": true`, boolean
/// only. A string, a number, a missing key, a non-object or malformed text read as
/// false. This is the only durable evidence of the mark — the block header's
/// anomaly bit is never persisted — so it is what the audit and the worker read.
/// Shares its reading with ShouldSkipIndex's metadata branch, and agrees with
/// kBlockSkipsIndexSql on every well-formed object with a single such key.
bool MetadataSkipsIndex(const std::string& metadata_json);

/// The same reading as a SELECT column over blocks.metadata_json: 1 when the mark
/// is the JSON value true, else 0 or NULL (read it with sqlite3_column_int, which
/// turns NULL into 0). CASE guards the malformed case — json_type raises on text
/// that is not JSON, and SQLite does not promise short-circuit evaluation of AND.
/// On a duplicated key json_type reads the first occurrence where the C++ parser
/// keeps the last; no writer emits duplicate keys (every writer stores a
/// serialized object), so the two readings agree on every stored row.
inline constexpr char kBlockSkipsIndexSql[] =
    "(CASE WHEN json_valid(metadata_json)"
    " THEN json_type(metadata_json, '$.\"cleaning.skip_index\"') END = 'true')";

}  // namespace cortrix::spc
