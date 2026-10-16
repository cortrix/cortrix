#pragma once
#include <cstdint>
#include <functional>
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
    std::string failed_members;  // csv of owed chain tokens, e.g. "f03,f38"
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
///   f03 ⇔ blocks.enriched_score non-NULL; f35 ⇔ contextualized_status == 1 AND,
///   where the ingest indexes the chunk at all, the contextual index label row
///   exists; f38 ⇔ a block_type=16 row whose metadata source_child_id == the
///   child, owed only where the ingest writes hype blocks at all.
///   "At all" is the ingest's own index gate (index_gates.h): IndexPointsExpected
///   (the document's processing level from its row, and the cleaner's skip-index
///   mark read with kBlockSkipsIndexSql) and HypeBlocksExpected (the level). The
///   backfill worker applies the same gate, so what the audit does not owe the
///   worker does not add.
/// Chunks already tracked as 'pending_retry' or 'failed_permanent' are left as
/// they are; fully-covered chunks get no row. Returns the synthesized count.
Result<int> SynthesizeEnrichAuditRows(
    sqlite3* db, const std::unordered_set<std::string>& configured_members,
    int64_t now_unix);

/// The other direction of the audit: contextual index labels — and the points
/// they resolve — that the ingest's rule excludes (index_gates.h: the chunk is
/// skip-index, or its document is below the full processing level). Such entries
/// were written by audits and backfills before the gates existed. For each, in
/// label order, `mark_point_deleted(label)` is called first (the index delete is
/// idempotent) and only then the label row removed, so a failure leaves a row the
/// next audit retries rather than a point nothing can resolve; the first failure
/// is returned and nothing after it is touched. The chunk's contextual columns,
/// its stored vector included, are left as they are. Should the gate ever admit
/// the chunk, the audit owes the contextual member again (unless the ledger
/// already tracks the chunk as pending or given up), and the backfill re-runs the
/// member through the provider, which re-adds the point under the same label and
/// writes the label row. Labels whose block no longer exists are not this
/// function's concern. An absent labels table reads as nothing to remove.
/// Returns the number removed.
Result<int> ReconcileExcludedContextualLabels(
    sqlite3* db, const std::function<Status(uint64_t label)>& mark_point_deleted);

}  // namespace cortrix::spc
