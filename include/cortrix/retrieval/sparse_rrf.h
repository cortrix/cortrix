#pragma once
#include <string>
#include <vector>

#include "cortrix/id/types.h"
#include "cortrix/retrieval/sparse_retriever.h"  // SparseHit, ChildId

namespace cortrix::retrieval {

/// Default RRF constant (k=60, industry default / NS-configurable
/// Phase 2).
inline constexpr int kRrfKDefault = 60;

/// The 5 chunk-level recall paths. via_path / explain uses
/// these names; the index order is the stable label order.
enum class RrfPath {
    kDense = 0,           ///< original child embedding (BGE-M3 dense)
    kContextualized,      ///< contextualized_embedding (dual-vector ANN hit)
    kSparse,              ///< sparse_vec → SpladeSparseRetriever.Search
    kFts5,                ///< literal BM25
    kHypeQuestion,        ///< hype_question hit expanded to its source child
};
inline constexpr int kRrfPathCount = 5;
const char* ToString(RrfPath path);

/// One fused result. `rrf_score` = Σ 1/(k+rank) over the paths that
/// ranked this child; `contributing_paths` is the B-class explain detail (which
/// of the 5 paths contributed, in label order).
struct RrfFusedHit {
    ChildId child_id;
    float rrf_score = 0.0f;
    std::vector<RrfPath> contributing_paths;
};

/// The 5 per-path ranked candidate lists (each already sorted by its own score
/// DESC; only rank position matters to RRF). A path that did not run / produced
/// nothing is an empty list — e.g. an un-enriched namespace feeds contextualized
/// and hype empty, an L2 sparse fallback drops the sparse list, and
/// simple/chat routes stay chunk-only.
struct FivePathInput {
    std::vector<SparseHit> dense;
    std::vector<SparseHit> contextualized;  ///< dual-vector votes
    std::vector<SparseHit> sparse;
    std::vector<SparseHit> fts5;
    std::vector<SparseHit> hype;            ///< hype votes for their source child
};

/// chunk-level 5-path RRF fusion. For each path, a candidate's
/// contribution is 1/(k + rank) where rank is its 0-based position in that path's
/// list (so the formula 1/(k+rank); rank starts at 0 here → the top hit
/// contributes 1/k). Results are deduped by child_id (scores summed across
/// paths), sorted by rrf_score DESC, truncated to `top_n` (<=0 → all).
///
/// This is the pure fusion function; the producing retrievers are wired in the
/// live executor's vector-route split (live_single_unit_executor.cpp, W2).
std::vector<RrfFusedHit> FuseFivePathRrf(const FivePathInput& input,
                                         int top_n = 0, int k = kRrfKDefault);

/// B-class explain payload for a sparse-path query
/// (`?explain=true`). via_path is "sparse" on the normal path
/// and a fallback token on L2; fallback_paths lists what actually ran.
struct SparseExplain {
    std::string via_path;                  ///< "sparse" | "fallback_dense_fts5_hype"
    std::vector<std::string> fallback_paths;  ///< paths that actually contributed
    int sparse_top_k = 0;                  ///< effective resolved top-K (S7)
    bool fallback_used = false;
};

}  // namespace cortrix::retrieval
