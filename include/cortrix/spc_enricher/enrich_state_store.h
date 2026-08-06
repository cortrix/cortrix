#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::spc {

/// Free functions over the per-Unit `enrich_state` sidecar table
/// (EnrichStateSchemaProvider). One row per enriched-eligible child block; the
/// write phase upserts the ingest outcome, the retry sweeper reads due docs, the
/// backfill worker flips rows as members are repaired. All helpers are plain
/// statements over a borrowed sqlite3* (same convention as enricher_store.h) so
/// they are unit-testable against an in-memory DB with the migration applied.

inline constexpr char kEnrichStatusOk[] = "ok";
inline constexpr char kEnrichStatusPendingRetry[] = "pending_retry";
inline constexpr char kEnrichStatusFailedPermanent[] = "failed_permanent";

/// First retry delay stamped by the ingest write phase when a chunk lands with
/// enrichment debt; the backfill worker owns the subsequent exponential schedule.
inline constexpr int kEnrichRetryFirstDelaySec = 60;

struct EnrichStateRow {
    uint64_t block_id = 0;
    std::string doc_id;
    std::string child_id;
    std::string status;          // one of the kEnrichStatus* constants
    std::string failed_members;  // csv of owed chain tokens, e.g. "enrich,hype"
    int attempts = 0;
    std::string last_error;
    int64_t next_retry_at = 0;   // unix seconds; 0 ⇔ SQL NULL (non-pending rows)
    int64_t updated_at = 0;      // unix seconds; caller stamps (testable clock)
};

/// Insert-or-replace the row for row.block_id (all fields overwritten).
Status UpsertEnrichState(sqlite3* db, const EnrichStateRow& row);

/// All rows for one document, ordered by block_id. status_filter: exact match,
/// empty = all statuses.
Result<std::vector<EnrichStateRow>> ListEnrichStateForDoc(
    sqlite3* db, const std::string& doc_id, const std::string& status_filter);

/// Distinct doc_ids that have at least one due 'pending_retry' row
/// (next_retry_at <= now_unix), oldest due first, capped at limit.
Result<std::vector<std::string>> ListDueDocs(sqlite3* db, int64_t now_unix, int limit);

/// Sweeper lease: push next_retry_at of every 'pending_retry' row of doc_id to
/// lease_until so the next sweep tick does not re-enqueue a doc whose backfill
/// task is already queued/running. The worker overwrites the real value when it
/// finishes.
Status LeaseDocRetries(sqlite3* db, const std::string& doc_id, int64_t lease_until,
                       int64_t now_unix);

struct EnrichStateCounts {
    int64_t total = 0;
    int64_t ok = 0;
    int64_t pending_retry = 0;
    int64_t failed_permanent = 0;
};

/// Coverage counters for the namespace (ops API / metrics gauge source).
Result<EnrichStateCounts> CountEnrichStates(sqlite3* db);

/// addendum §3.7 G4 — legacy/silent-degrade audit. Scans the namespace's child
/// blocks against the CONFIGURED chain members and synthesizes 'pending_retry'
/// rows (due at now_unix) for chunks whose artifacts are missing:
///   enrich ⇔ blocks.enriched_score non-NULL; contextual ⇔ contextualized_status == 1;
///   hype ⇔ a block_type=16 row whose metadata source_child_id == the child.
/// Chunks already tracked as 'pending_retry' keep their attempts/backoff (not
/// clobbered); fully-covered chunks get no row. Returns the synthesized count.
Result<int> SynthesizeEnrichAuditRows(
    sqlite3* db, const std::unordered_set<std::string>& configured_members,
    int64_t now_unix);

}  // namespace cortrix::spc
