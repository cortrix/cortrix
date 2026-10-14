#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/common/data_types.h"        // CortrixParent
#include "cortrix/query/cross_ns_response.h"  // CrossNsResponse

// F34 §2.5 / F41 §8.1 bounded parent expansion, resolved against the FINAL response.
//
// Why this lives outside the per-namespace executor: which results get parent text
// has to be decided from the order the caller actually receives. Between a namespace
// producing its hits and the response being serialized, three stages re-order or drop
// results — RAG-fusion re-ordering, the LLM listwise rerank plus its trim to the
// requested top_k, and CRAG's halving on an ambiguous verdict. Choosing inside a
// namespace therefore expands a hit that may end up ranked lower, or dropped, while
// the hit the caller sees first carries no parent text. A later budget pass cannot
// repair that: it can only clear text, never fetch it.
//
// The work splits into three steps so the two that need no store are directly
// testable, and so the store-facing one stays at the layer that owns the namespace
// pool (query_wiring):
//
//   1. SelectParentExpansionTargets — pure. Final order in, targets out.
//   2. (caller) fetch those parents, one facade acquire per distinct namespace.
//   3. ApplyParentExpansion — pure. Assigns text under one response-wide budget.
namespace cortrix::query {

/// One result selected for expansion, carrying where to look the parent up. The
/// namespace is part of the identity: parent ids are unique within a namespace's
/// store, not across the response.
struct ParentExpansionTarget {
    std::string namespace_id;
    std::string parent_id;
    std::size_t result_index = 0;  ///< index into CrossNsResponse::results (final order)
};

/// (namespace_id, parent_id) → parent row. Namespace-qualified for the reason above.
using ParentTextByKey = std::map<std::pair<std::string, std::string>, CortrixParent>;

/// Step 1 (pure). Walk `resp.results` in final order and select the first
/// `expand_top_k` results that carry a parent_id. Results without one (document-level
/// pseudo-results, legacy rows) are skipped without consuming depth — they can never
/// be expanded, so letting them use up the window would silently shrink it.
/// `expand_top_k <= 0` selects nothing.
std::vector<ParentExpansionTarget> SelectParentExpansionTargets(const CrossNsResponse& resp,
                                                                int expand_top_k);

/// Fetch one namespace's parents. Rows that are absent (dangling link, or a store
/// without parent support) are simply left out of the returned vector.
using NamespaceParentFetch =
    std::function<std::vector<CortrixParent>(const std::string& namespace_id,
                                             const std::vector<std::string>& parent_ids)>;

/// Step 2. Group `targets` by namespace and ask `fetch` for each namespace's DISTINCT
/// parent ids, once per namespace. Keeping the store behind a callback is deliberate:
/// it lets the caller hold a namespace facade for exactly the span of that namespace's
/// lookups (RAII, no dangling store pointer), and lets this step be tested without one.
/// Two results sharing a parent cost one lookup; they still cost two copies on the
/// wire, which is why the budget in step 3 is charged per result.
ParentTextByKey FetchParentsForTargets(const std::vector<ParentExpansionTarget>& targets,
                                       const NamespaceParentFetch& fetch);

/// Step 3 (pure). Assign parent text to `targets` in order, under ONE response-wide
/// byte budget, and write the three expansion-metadata keys alongside each one.
///
/// Prefix semantics: the first target whose text does not fit `max_bytes` stops the
/// pass, so the expanded set is always a prefix of the final order. Skipping it and
/// admitting a smaller, lower-ranked parent instead would reproduce in miniature the
/// defect this file exists to fix — the caller's top hit lacking what a lower one has.
/// A target whose parent is absent from `parents` (dangling link, or a store without
/// parent support) is skipped and does NOT stop the pass: a broken link must degrade
/// the expansion, never truncate it for everyone below.
///
/// `max_bytes <= 0` means no byte cap; depth is still bounded by the target list.
/// Returns the number of results that received parent text.
int ApplyParentExpansion(CrossNsResponse& resp,
                         const std::vector<ParentExpansionTarget>& targets,
                         const ParentTextByKey& parents,
                         int max_bytes);

}  // namespace cortrix::query
