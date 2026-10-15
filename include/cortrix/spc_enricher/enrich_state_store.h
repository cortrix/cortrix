#pragma once
#include <cstdint>
#include <limits>
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

/// Provider-attempt cap: the backfill worker gives up on a chunk once this many
/// deliveries have been counted and the artifact is still owed.
inline constexpr int kEnrichMaxAttempts = 8;

/// Give-up bound that no count ever reaches. Passed where a delivery costs no
/// provider call and must therefore never end a row: label-only contextual debt,
/// where the columns and the stored vector exist and only the index label is
/// missing (kBlockHasStoredContextualVectorSql in cortrix/spc/contextual_store.h).
inline constexpr int kEnrichNoTerminal = std::numeric_limits<int>::max();

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

/// Operator backfill: make every pending row due at `now_unix`. Rows that are
/// already due are left untouched — rewriting them would give every document the
/// same due time, and the sweep that follows would then serve the same first
/// batch on every call while the rest wait for ever (#149). Returns rows changed.
Result<int> MakeAllPendingDueNow(sqlite3* db, int64_t now_unix);

/// Count one delivery against these pending rows, before the first provider call.
/// The worker's end-of-run flip is the only other writer of `attempts`, and a
/// task that dies before the flip (a storage failure after the provider ran, a
/// handler that throws, a worker crash) left the count untouched — so the cap
/// could never be reached on exactly the paths it exists for. A row whose new
/// count exceeds `terminal_at_attempts` is given up on here. Provider deliveries
/// pass kEnrichMaxAttempts + 1: one over the flip's cap, so a row that has spent
/// its attempts gets exactly one further delivery to heal (a crash after a
/// successful last attempt) before the entry charge ends it. Label-only
/// deliveries pass kEnrichNoTerminal: they call no provider, so the cap does not
/// apply to them, but the delivery is still counted. Returns rows changed.
Result<int> ChargeEnrichAttempts(sqlite3* db, const std::vector<uint64_t>& block_ids,
                                 int64_t now_unix, int terminal_at_attempts);

/// Audit re-owe. A new row is inserted as given. An existing row KEEPS its
/// attempts (the audit observes a missing artifact; it did not deliver anything)
/// and, once its attempts have reached `terminal_at_attempts`, is marked
/// failed_permanent instead of being re-owed — a row that spent its attempts is
/// not resurrected. Pass kEnrichNoTerminal to exempt the row, which the audit
/// does when the only missing artifact is the contextual index label while the
/// columns and the stored vector are present: the provider work is done, and one
/// more delivery re-adds the point from the stored vector and writes the label.
Status ReoweEnrichState(sqlite3* db, const EnrichStateRow& row, int terminal_at_attempts);

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
///   f03 ⇔ blocks.enriched_score non-NULL; f35 ⇔ contextualized_status == 1 AND
///   the contextual index label row exists; f38 ⇔ a block_type=16 row whose
///   metadata source_child_id == the child.
/// Chunks already tracked as 'pending_retry' keep their attempts/backoff (not
/// clobbered). An 'ok' chunk with a missing artifact is re-owed with its count
/// kept and is given up on at kEnrichMaxAttempts. 'failed_permanent' chunks stay
/// terminal, with one exception: when the contextual columns and the stored
/// vector are present and only the index label is missing
/// (kBlockHasStoredContextualVectorSql), the repair costs no provider call, so
/// the chunk is re-owed whatever its count. Fully-covered chunks get no row.
/// Returns the synthesized count.
Result<int> SynthesizeEnrichAuditRows(
    sqlite3* db, const std::unordered_set<std::string>& configured_members,
    int64_t now_unix);

}  // namespace cortrix::spc
