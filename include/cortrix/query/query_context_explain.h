#pragma once
#include <nlohmann/json.hpp>

#include "cortrix/query/query_context.h"

namespace cortrix::query {

/// Serialize a QueryContext to the `meta.query_context` node of the ARCH
/// `?explain=true` response (the query-context spec). JSON keys are snake_case,
/// 1:1 with the struct fields (SPEC — no camelCase mapping layer).
///
/// All 13 SPEC fields are emitted with their values (SPEC: default-valued
/// fields are returned as ""/0.0/false/{}, never omitted, so an Agent never sees a
/// missing field). The phased-rollout A/B/C visibility split (which fields the
/// real endpoint exposes by default vs only under ?explain=true) is the endpoint's
/// concern; this helper produces the full node and the endpoint filters as needed.
///
/// 🚨 standalone: the real `GET /api/v1/query?...&explain=true` endpoint does
/// not exist in the frozen tree (ARCH-owned, cross-Feature → integration). This is the
/// dump helper that endpoint will call; it is fully usable + testable in-process.
/// First needed by the CRAG verdict fields; the router reuses it for the routing
/// fields.
nlohmann::json ToExplainJson(const QueryContext& ctx);

/// Build the `?explain=true` node with the query-context spec phased-rollout
/// A/B/C visibility split applied (the gating ToExplainJson deliberately leaves to
/// the endpoint):
///   - A class (data integrity) — always present.
///   - B class (LLM-dependent decision path) — present (this node is only built when
///     the caller already decided `?explain=true`).
///   - C class (anomaly debug: crag_signals / routing_misclassified) — present only
///     when `include_debug` is true, i.e. the corresponding capability ran AND
///     failed (SPEC C-class rule). Otherwise the C fields are omitted so an Agent
///     does not mistake a default-valued signal for a real one.
///
/// JSON keys are snake_case, 1:1 with the struct fields (SPEC). This is what
/// the real `POST /api/v1/query?explain=true` route attaches as the top-level
/// `explain` object.
nlohmann::json BuildExplainNode(const QueryContext& ctx, bool include_debug);

}  // namespace cortrix::query
