#pragma once
#include <cstdint>

#include <sqlite3.h>

#include "cortrix/common/status.h"

namespace cortrix::scoring {

/// Persists the F07 write-time semantic_score into the per-Unit blocks column the
/// ScoringSchemaProvider creates (ARCH §5.1.2 / F07 §3). A free function over a
/// sqlite3* (mirrors spc::WriteEnrichment for the F03 enriched_score column) so it is
/// unit-testable against an in-memory DB with the F07 migration applied; the live
/// wiring (which block_id, the F25 write boundary) lives in the SPC pipeline (D3.5).
///
/// `semantic_score` is the discrete 5-level score (0.2/0.4/0.6/0.8/1.0) from
/// ScoreMap::LevelToScore, OR the D6 anomaly sentinel 0.0 — both are real values worth
/// persisting (the column default NULL means "F07 never ran for this block").
Status WriteScore(sqlite3* db, uint64_t block_id, float semantic_score);

}  // namespace cortrix::scoring
