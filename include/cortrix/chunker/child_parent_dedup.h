#pragma once
#include <string>
#include <vector>

#include "cortrix/id/types.h"

// Child→parent dedup + the response meta
// types it produces.
//
// P-HNSW top-K returns child hits, several of which may share one parent. The C
// top-3 strategy keeps at most `children_per_parent_for_rerank` (default 3,
// GUC-/NS-tunable 1-10) children per parent to feed the reranker; the rest are
// recorded in `meta.children_hits_per_parent` (result completeness, not dropped
// silently). Standalone: the real P-HNSW search + rerank wiring lands later; this
// is the pure grouping/selection given hits whose parent_id is already attached.
namespace cortrix::chunker {

/// One P-HNSW hit (vec metadata already carries parent_id, step 2). For
/// flat-fallback children parent_id may be empty — each such hit is its own group.
struct ChildHit {
    cortrix::id::ChildId  child_id;
    cortrix::id::ParentId parent_id;
    float score = 0.0f;
};

/// One parent's deduped hit set after C top-3 ( step 3).
struct ParentHitGroup {
    cortrix::id::ParentId parent_id;
    cortrix::id::ChildId  primary_child_id;          ///< highest-scoring child of this parent
    std::vector<cortrix::id::ChildId> rerank_child_ids;  ///< ≤ children_per_parent_for_rerank, score desc
    std::vector<cortrix::id::ChildId> all_hit_child_ids; ///< every child of this parent that was hit, score desc
    float top_score = 0.0f;                          ///< primary child's score (group order key)
};

/// The `meta.children_hits_per_parent[]` entry.
struct ChildrenHitsPerParent {
    cortrix::id::ParentId parent_id;
    std::vector<cortrix::id::ChildId> hits;      ///< all hit child_ids (score desc)
    cortrix::id::ChildId primary_child;          ///< highest-scoring child
};

/// Group child hits by parent and apply C top-3.
///
/// - Hits are grouped by parent_id; within each group, children are ordered by
///   score desc (ties broken by child_id for determinism).
/// - `rerank_child_ids` = the first min(group_size, children_per_parent) children
///   (these feed the reranker, step 4).
/// - `primary_child_id` = the top-scored child.
/// - Output groups are ordered by top_score desc (best parent first).
/// - A hit with empty parent_id (flat fallback) forms a singleton group keyed by
///   its child_id, so flat results pass through unchanged.
///
/// `children_per_parent_for_rerank` is clamped to ≥ 1 (a 0 would drop all
/// children — never intended; GUC validation already enforces [1,10]).
std::vector<ParentHitGroup> DedupChildrenToParents(
    const std::vector<ChildHit>& hits, int children_per_parent_for_rerank);

/// Build the `meta.children_hits_per_parent` from deduped groups (records
/// EVERY hit child per parent, not just the reranked subset — result completeness).
/// Only multi-child parents are typically interesting, but all groups are emitted
/// for a faithful hit manifest.
std::vector<ChildrenHitsPerParent> BuildChildrenHitsMeta(
    const std::vector<ParentHitGroup>& groups);

}  // namespace cortrix::chunker
