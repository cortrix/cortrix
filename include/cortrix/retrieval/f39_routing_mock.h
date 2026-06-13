#pragma once
#include "cortrix/query/query_context.h"

namespace cortrix::retrieval {

/// ShouldSkipF37 — MOCK of F39's routing-skip decision (C-R1 briefing §2 /
/// F37 §4.1 v1.0.3 M2 / F39 §5.1).
///
/// 🚨 STANDALONE MOCK — NOT the real owner. The real decision is
/// `cortrix::query::QueryComplexityClassifier::ShouldSkipF37(const QueryContext&)`,
/// a static helper the QueryPipeline calls *before* invoking F37 (F37 itself never
/// decides skip — §4.1 v1.0.3 M2). F39's QueryComplexityClassifier is NOT frozen
/// this round (it lands in Wave C-R2), so F37 provides this mock to (a) make the
/// skip semantics testable standalone and (b) give a stable call shape. When F39
/// freezes, the QueryPipeline calls F39's real helper and this mock is dropped
/// (D3.5 wiring) — it is intentionally NOT referenced by CragEvaluator.
///
/// Mock rule (per briefing — decide by the routing_path value): F37 runs for the Complex /
/// (default) path; it is skipped on the Simple and Chat paths. Chat skips all
/// retrieval (F39-5); Simple keeps 4-way RRF but skips the F36/F37/F38-hype
/// refinements (F39-3). An empty routing_path (not yet routed) → do NOT skip
/// (default to running F37, matching F39's fail-safe "default Complex").
inline bool ShouldSkipF37(const query::QueryContext& ctx) {
    if (ctx.chat_path_triggered) return true;           // Chat: no retrieval eval
    if (ctx.routing_path == "simple") return true;      // Simple: skip CRAG refinement
    if (ctx.routing_path == "chat") return true;        // Chat (via routing_path)
    return false;                                       // complex / "" → run F37
}

}  // namespace cortrix::retrieval
