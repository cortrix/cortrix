#pragma once
#include <string>

#include "cortrix/common/status.h"

namespace cortrix::catalog::gc {

/// Stage 3 physical-deletion sink (OPEN-2 GC). The blob_gc_queue is keyed by
/// `blob_uri` (content-addressed `{hash[0:2]}/{hash}`, cross-NS/Unit), which does
/// not map onto the MVP per-(ns, doc_id) blob store. GcManager therefore drives
/// Stage 3 through this abstraction instead of the MVP store: the Phase 1 default
/// (LocalBlobGcSink) resolves a blob_uri under a content-addressed base dir and
/// unlinks it; Phase 2 swaps in S3 / NFS deleters without touching the manager.
///
/// Return convention mirrors the catalog Status model: Ok() on success (including
/// "already gone" — unlink of a missing file is not an error, the queue row is
/// still retired), an error Status on a real IO failure (the row stays pending
/// for the next sweep).
class IBlobGcSink {
public:
    virtual ~IBlobGcSink() = default;

    /// Physically delete the blob identified by `blob_uri`. Idempotent: deleting
    /// an already-absent blob returns Ok().
    virtual Status Unlink(const std::string& blob_uri) = 0;
};

/// No-op sink: records nothing and always succeeds. Used when blob storage is not
/// configured (queue rows are still retired so the catalog converges) and as a
/// test double.
class NullBlobGcSink : public IBlobGcSink {
public:
    Status Unlink(const std::string& /*blob_uri*/) override { return Status::Ok(); }
};

/// Phase 1 local sink: treats `blob_uri` as a path relative to `base_dir`
/// (`{base_dir}/{blob_uri}`) and unlinks it. A missing file is Ok() (idempotent).
class LocalBlobGcSink : public IBlobGcSink {
public:
    explicit LocalBlobGcSink(std::string base_dir);

    Status Unlink(const std::string& blob_uri) override;

private:
    std::string base_dir_;
};

}  // namespace cortrix::catalog::gc
