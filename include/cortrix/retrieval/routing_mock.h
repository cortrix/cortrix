#pragma once
#include "cortrix/query/query_context.h"

namespace cortrix::retrieval {

/// ShouldSkipCrag — MOCK of the router's skip decision
/// used until the real router lands.
///
/// 🚨 STANDALONE MOCK — NOT the real owner. The real decision is
/// `cortrix::query::QueryComplexityClassifier::ShouldSkipCrag(const QueryContext&)`,
/// a static helper the QueryPipeline calls *before* invoking CRAG (CRAG itself never
/// decides skip). QueryComplexityClassifier is NOT frozen
/// this round, so CRAG provides this mock to (a) make the
/// skip semantics testable standalone and (b) give a stable call shape. When the router
/// freezes, the QueryPipeline calls its real helper and this mock is dropped
/// (D3.5 wiring) — it is intentionally NOT referenced by CragEvaluator.
///
/// Mock rule (decide by the routing_path value): CRAG runs for the Complex /
/// (default) path; it is skipped on the Simple and Chat paths. Chat skips all
/// retrieval; Simple keeps 4-way RRF but skips the RAG-Fusion, CRAG and HyPE
/// refinements. An empty routing_path (not yet routed) → do NOT skip
/// (default to running CRAG, matching the router's fail-safe "default Complex").
inline bool ShouldSkipCrag(const query::QueryContext& ctx) {
    if (ctx.chat_path_triggered) return true;           // Chat: no retrieval eval
    if (ctx.routing_path == "simple") return true;      // Simple: skip CRAG refinement
    if (ctx.routing_path == "chat") return true;        // Chat (via routing_path)
    return false;                                       // complex / "" → run CRAG
}

}  // namespace cortrix::retrieval
