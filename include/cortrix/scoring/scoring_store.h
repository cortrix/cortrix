#pragma once
#include <cstdint>

#include <sqlite3.h>

#include "cortrix/common/status.h"

namespace cortrix::scoring {

/// Persists the write-time semantic_score into the per-Unit blocks column the
/// ScoringSchemaProvider creates. A free function over a
/// sqlite3* (mirrors spc::WriteEnrichment for the enriched_score column) so it is
/// unit-testable against an in-memory DB with the scoring migration applied; the live
/// wiring (which block_id, the coordinated write boundary) lives in the SPC pipeline.
///
/// `semantic_score` is the discrete 5-level score (0.2/0.4/0.6/0.8/1.0) from
/// ScoreMap::LevelToScore, OR the D6 anomaly sentinel 0.0 — both are real values worth
/// persisting (the column default NULL means "scoring never ran for this block").
Status WriteScore(sqlite3* db, uint64_t block_id, float semantic_score);

}  // namespace cortrix::scoring
