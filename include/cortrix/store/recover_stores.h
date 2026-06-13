#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/common/types.h"  // BlockId

namespace cortrix::store {

/// Minimal consumer-side (DIP) contracts the F25 WriteCoordinator depends on for
/// the three-way crash-recovery consistency check (design § 4.4, Q1-C). Defined
/// here on the *consumer* side so Recover() can be unit-tested against mocks —
/// reproducing a crash mid-write requires each store's "does this exist?" answer
/// to be programmable, which a concrete MVP store cannot offer (D3 decision
/// 2026-05-30). Real MVP adapters (`BlockExists ≡ block_get != not_found`,
/// `BlobExists ≡ load != not_found`) are wired at D3.5 L0 integration.
///
/// The vector side uses F01's richer IVectorStore (cortrix/store/i_vector_store.h,
/// whose Exists() is the canonical membership check) — F25 does not redefine it.
/// Ownership note: these two interfaces are F25-consumer-defined for L0; if F08
/// Metadata Block later introduces a canonical metadata-store contract, they are
/// reconciled then.

/// Metadata store membership for Recover (SQLite blocks table in MVP).
class IMetadataStore {
public:
    virtual ~IMetadataStore() = default;

    /// True iff every block_id is present. Used by Recover's three-way check:
    /// a "PENDING-only" txn whose blocks all exist (alongside vector + blob) is
    /// inferred COMMITTED; any missing block triggers rollback cleanup (Q1-C).
    virtual bool BlockExists(const std::vector<BlockId>& block_ids) = 0;
};

/// Blob store membership for Recover (per-doc blob file in MVP).
class IBlobStore {
public:
    virtual ~IBlobStore() = default;

    /// True iff the document's blob is present (design § 4.4 Blob.BlobExists).
    virtual bool BlobExists(const std::string& doc_id) = 0;
};

}  // namespace cortrix::store
