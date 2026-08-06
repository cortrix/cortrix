#pragma once
#include <string>
#include <vector>

#include "cortrix/chunker/types.h"  // cortrix::chunker::ParentChunk (SoT)
#include "cortrix/common/result.h"

// ParentChunkStore — parents-table data-access layer (Repository pattern,
// detailed design, V3 ruling 2).
//
// Single-point + batch lookup of parent_text for downstream Enrichers reverse-
// looking-up context: HyPEEnricher (GetParent), doc-summary /
// ExpandParentText (BulkGetParents). Dependency-injected + mock-friendly so
// those callers unit-test against this interface (parallel to the reranker's ChunkStore,
// which is a *child*-level lookup — separate class, not a base of this one).
//
// Error model (briefing template B): fallible ops return Result<T> (StatusOr); the
// domain identity is a CX_ERR_STORE_* code carried on the Status message via
// store/store_errors.h::StoreStatus(). NOT Result<T, StoreError>.
namespace cortrix::store {

class ParentChunkStore {
public:
    virtual ~ParentChunkStore() = default;

    /// Single-point lookup (HyPEEnricher). Returns:
    ///   Ok(ParentChunk)                      — hit
    ///   CX_ERR_STORE_NOT_FOUND (kNotFound)   — parent_id absent
    ///   CX_ERR_STORE_DB_ERROR  (kInternal)   — underlying DB error
    virtual Result<cortrix::chunker::ParentChunk> GetParent(
        const std::string& parent_id) = 0;

    /// Batch lookup (ExpandParentText top-K / post-RRF batch reverse-
    /// lookup). Partial hits still succeed: missing parent_ids are simply absent
    /// from the result (the caller detects them). Returns:
    ///   Ok(vector<ParentChunk>)              — order matches input; misses dropped
    ///   CX_ERR_STORE_DB_ERROR  (kInternal)   — underlying DB error
    virtual Result<std::vector<cortrix::chunker::ParentChunk>> BulkGetParents(
        const std::vector<std::string>& parent_ids) = 0;
};

}  // namespace cortrix::store
