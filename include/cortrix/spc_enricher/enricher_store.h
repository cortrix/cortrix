#pragma once
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/common/status.h"
#include "cortrix/spc_enricher.h"  // EnrichResult, Entity

namespace cortrix::spc {

/// Persists an EnrichResult into the per-Unit DB schema F03SchemaProvider creates
/// (design §3.1, S5.4). Free functions over a sqlite3* so they are unit-testable
/// against an in-memory DB with the F03 migration applied; the live wiring (which
/// block_id, transaction boundaries with F09 block writes) is cross-Feature → D3.5.
///
/// Field placement (design §3.1 field classification):
///   - enriched_score / enriched_at → blocks columns (A-class, query-returned)
///   - enricher_metadata (prompt_version / model_used / enricher_name JSON) →
///     blocks.enricher_metadata (B-class audit; query default hides it, §3.1)
///   - summary → caller writes into Block.payload.metadata JSONB (F09-owned; not
///     here — this module owns only the F03 columns + entities table)
///   - entities[] → entities table + entities_fts FTS5 index

/// Update the F03 block columns (enriched_score / enriched_at / enricher_metadata)
/// for `block_id`. status != 0 results skip the score/metadata (nothing to write).
Status WriteBlockEnrichment(sqlite3* db, uint64_t block_id, const EnrichResult& result);

/// Insert `entities` for `block_id` into the entities table + keep entities_fts in
/// sync (external-content FTS5 needs an explicit companion insert). Empty → no-op.
Status WriteEntities(sqlite3* db, uint64_t block_id, const std::vector<Entity>& entities);

/// Convenience: write block columns + entities for one (block_id, result) in a
/// single transaction. A failed/empty result still updates nothing destructive.
Status WriteEnrichment(sqlite3* db, uint64_t block_id, const EnrichResult& result);

/// Read back the entities for a block (test/debug helper + the read side the
/// query path will use). Ordered by entity_id.
std::vector<Entity> ReadEntities(sqlite3* db, uint64_t block_id);

}  // namespace cortrix::spc
