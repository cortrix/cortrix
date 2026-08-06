#pragma once
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/id/types.h"          // ID type SoT (ARCH)
#include "cortrix/retrieval/sparse_codec.h"  // SparseVector

namespace cortrix::retrieval {

using cortrix::id::ChildId;       ///< ULID TEXT — SoT cortrix::id (ARCH)
using cortrix::id::NamespaceId;   ///< NS string — SoT cortrix::id

/// One sparse-retrieval candidate. SoT for the retrieval-link sparse
/// hit type is the retrieval-types spec; this is the in-tree realization. The
/// `child_id` aligns with children.child_id (renamed
/// block_id→child_id). `score` is the SPLADE dot-product accumulator value.
struct SparseHit {
    ChildId child_id;
    float   score = 0.0f;
};

/// Sparse retriever interface (Cortrix consistent interface-reservation pattern,
/// like IIndexFactory / ConfigResolver). The SPLADE inverted-index
/// implementation is SpladeSparseRetriever (S5/S6); the interface exists so the
/// query pipeline + RRF code (integration wiring) depends on the abstraction, and so a
/// future IVF-sparse impl (Phase 2, N>100M) drops in without touching callers.
///
/// Errors are surfaced via cortrix::Status (F-FREEZE-1, no Result<T,E>); the
/// domain identity is carried by the CX_ERR_SPARSE_* token in the Status message
/// (re-inflatable to the Agent-friendly body via MakeSparseError). Search
/// returns a vector directly (an empty result is success, not an error); Add /
/// Remove return Status.
class ISparseRetriever {
public:
    virtual ~ISparseRetriever() = default;

    /// Query: top-`top_k` children for `query` within `ns_id`, by descending
    /// SPLADE score. A namespace with no indexed sparse vectors returns an empty
    /// vector (success). Implementations that hit an internal fault return an
    /// empty vector AND should let the caller observe the L2-fallback path
    /// — Search itself does not throw.
    virtual std::vector<SparseHit> Search(
        const SparseVector& query,
        const NamespaceId& ns_id,
        int top_k) = 0;

    /// Write/update the sparse vector for `child_id` in `ns_id` (incremental
    /// index). Re-adding an existing child_id replaces its postings.
    /// An empty `vec` removes any existing postings (the dead-chunk /
    /// path — equivalent to Remove). Returns CX_ERR_SPARSE_INVERTED_INDEX_WRITE_*
    /// on a write fault (retryable).
    virtual Status Add(
        const NamespaceId& ns_id,
        const ChildId& child_id,
        const SparseVector& vec) = 0;

    /// Delete all postings for `child_id` in `ns_id`. Removing an
    /// absent child_id is a no-op success (idempotent delete).
    virtual Status Remove(
        const NamespaceId& ns_id,
        const ChildId& child_id) = 0;

    /// Startup/health check — false when the backing store is
    /// unavailable, so the query layer routes around the sparse path (L2
    /// fallback) instead of failing the whole query.
    virtual bool IsAvailable() const = 0;
};

}  // namespace cortrix::retrieval
