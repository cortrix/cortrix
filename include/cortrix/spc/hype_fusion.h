#pragma once
#include <map>
#include <string>
#include <vector>

#include "cortrix/common/block_types.h"
#include "cortrix/id/types.h"

namespace cortrix::spc {

using cortrix::id::ChildId;
using cortrix::id::ParentId;

/// Default RRF constant (F38 §8.1: k=60, industry default / NS-config Phase 2).
inline constexpr int kHypeRrfK = 60;

/// One P-HNSW candidate in the mixed pool (F38 §8.1 ChildCandidate analog). A
/// chunk candidate (block_type != kBlockHypeQuestion — a content Block) scores
/// its own child_id; a hype candidate (block_type == kBlockHypeQuestion=16)
/// scores its source_child_id (the chunk it points to, from the Block metadata
/// source_child_id) and carries the matched question text for the B-class explain.
struct HypeCandidate {
    ChildId child_id;          ///< the candidate's own child_id (chunk path)
    int block_type = 0;        ///< kBlockHypeQuestion(16) for hype; any other = chunk path
    float score = 0.0f;        ///< P-HNSW similarity score (descending within a path)
    ChildId source_child_id;   ///< hype only: the chunk this question points to
    std::string question_text; ///< hype only: matched hypothetical question (explain)
};

/// Fused result with the 3 F38-9 B-class explain fields (§8.2).
struct HypeFusedResult {
    ChildId child_id;
    ParentId parent_id;        ///< for by-parent dedup (empty if unknown)
    float rrf_score = 0.0f;
    // --- B-class explain (?explain=true) ---
    bool via_hype = false;             ///< this chunk was (also) recalled via a hype question
    std::string hype_question_matched; ///< the best hype question that hit (empty if none)
    float hype_match_score = 0.0f;     ///< that hype candidate's raw score
};

/// chunk-level RRF + by-parent dedup over a mixed P-HNSW candidate pool.
///   Step 1: split by block_type → chunk path (child_id) + hype path (mapped to
///           source_child_id).
///   Step 2: RRF fuse the two rank lists (contribution 1/(k+rank) per path).
///   Step 3: by-parent dedup — among children sharing a parent_id, keep the one
///           with the highest rrf_score (§8.1 step 4). `parent_of` maps child_id
///           → parent_id; a child absent from the map is its own dedup group
///           (kept as-is, empty parent_id).
///   Step 4: sort by rrf_score DESC (child_id tie-break), truncate to top_n.
/// The 3 B-class explain fields are filled: via_hype + the best matched question
/// + its score for any chunk that got a hype contribution.
///
/// Standalone (D3): the real P-HNSW mixed-pool Search + the routing_path
/// block_type filter (simple/chat skip type 16, §8) + ExpandParentText +
/// QueryPipeline integration = D3.5; here the candidate pool + parent map are
/// provided directly.
std::vector<HypeFusedResult> FuseHypeRecall(
    const std::vector<HypeCandidate>& candidates,
    const std::map<ChildId, ParentId>& parent_of,
    int top_n = 0,
    int rrf_k = kHypeRrfK);

}  // namespace cortrix::spc
