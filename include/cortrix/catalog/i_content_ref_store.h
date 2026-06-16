#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::catalog {

/// dedup reference counting (F12 §3.3). Tracks which (file_hash, ns_id, doc_id)
/// triples reference a content-addressed file, backing OPEN-2 three-stage GC.
/// Maintains catalog.content_refs rows + the file_locations.ref_count aggregate.
///
/// Error model (F-FREEZE-1): value methods return Result<T> (error path = Status
/// with a CX_ERR_* token); void-fallible methods return Status.
class IContentRefStore {
public:
    virtual ~IContentRefStore() = default;

    /// Add a reference (dedup hit): upsert the content_refs row + bump
    /// file_locations.ref_count. Idempotent on an existing (file,ns,doc) triple.
    virtual Status IncrementRef(const std::string& file_hash,
                                const std::string& ns_id,
                                const std::string& doc_id) = 0;

    /// Remove a reference (delete): soft-delete the content_refs row + decrement
    /// ref_count; returns the remaining ref_count for the file. Error:
    /// CX_ERR_REF_COUNT_NEGATIVE if the count would go below zero.
    virtual Result<int> DecrementRef(const std::string& file_hash,
                                     const std::string& ns_id,
                                     const std::string& doc_id) = 0;

    /// Current reference count for a file (0 if unknown).
    virtual Result<int> GetRefCount(const std::string& file_hash) const = 0;

    /// OPEN-2 GC Stage 3: file_hashes whose ref_count has been 0 since before
    /// `before_timestamp_ms` (candidates for physical blob deletion).
    virtual Result<std::vector<std::string>> GetGCCandidates(
        int64_t before_timestamp_ms) const = 0;
};

}  // namespace cortrix::catalog
