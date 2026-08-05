#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/import/import_types.h"

namespace cortrix::import {

/// SPC-pipeline seam (feed_to_spc_pipeline). A TextChunk → one Block
/// through the SPC pipeline (Parse→Chunk→Embed→Assemble→Store).
///
/// ⚠️ Real-contract note: the frozen pipeline class is `cortrix::SPCPipeline` (spc/
/// spc_pipeline.h — all-caps, NO `I` prefix, NO abstract base), with concrete ctor
/// deps + `int Process(SPCTask&, CortrixNamespace&)`. It is heavyweight and not
/// directly mockable, so the importer consumes it behind THIS thin `ISpcFeeder` seam:
/// standalone uses InMemorySpcFeeder; the real adapter (TextChunk → SPCTask →
/// SPCPipeline::Process against a live CortrixNamespace) is wired in D3.5.
class ISpcFeeder {
public:
    virtual ~ISpcFeeder() = default;

    /// Feed one batch of textualized chunks into namespace `ns`. Returns the number
    /// of chunks successfully ingested (== chunks.size() on full success); an error
    /// Status aborts the batch. Each chunk's source URI + metadata (§4.3) flow into
    /// the resulting Block.
    virtual Result<int> Feed(const std::vector<TextChunk>& chunks, const NsId& ns) = 0;
};

/// Standalone SPC feeder — records what it would ingest (no real embedding / store).
/// Lets the import-flow unit tests assert "the right chunks, with the right source
/// URIs + metadata, reached the pipeline" without a live SPCPipeline. Real ingest →
/// D3.5. Thread-safe.
class InMemorySpcFeeder : public ISpcFeeder {
public:
    Result<int> Feed(const std::vector<TextChunk>& chunks, const NsId& ns) override;

    struct FedBatch {
        NsId ns;
        std::vector<TextChunk> chunks;
    };
    std::vector<FedBatch> batches() const;
    int total_chunks() const;

private:
    mutable std::mutex mu_;
    std::vector<FedBatch> batches_;
};

/// Full-overwrite seam (cleanup_source_blocks). Clears only the
/// Blocks in `ns` whose metadata_json `source` begins with `source_prefix` — i.e.
/// the prior import of this exact table (ARCH §3.6 lock: only clears Blocks sourced from this table).
///
/// Real impl issues `DELETE FROM blocks WHERE ns_id=? AND metadata_json->>'source'
/// LIKE '<prefix>%'` against the namespace store → D3.5; standalone uses
/// InMemoryBlockCleaner.
class IBlockCleaner {
public:
    virtual ~IBlockCleaner() = default;

    /// Delete this table's prior Blocks. Returns the count removed (for the audit /
    /// progress record). source_prefix MUST end in '/' (the table boundary) so
    /// "users/" never clears "users2/" (R6 accidental-deletion mitigation).
    virtual Result<int> CleanupSourceBlocks(const NsId& ns,
                                            const std::string& source_prefix) = 0;
};

/// Standalone block cleaner — models a set of Block source URIs per namespace so the
/// D3 prefix-match semantics (and the "users/ ≠ users2/" boundary) are unit-tested
/// without a live store. Real DELETE → D3.5. Thread-safe.
class InMemoryBlockCleaner : public IBlockCleaner {
public:
    /// Seed a Block source URI (test setup — pretend a prior import wrote it).
    void SeedBlock(const NsId& ns, const std::string& source_uri);

    Result<int> CleanupSourceBlocks(const NsId& ns, const std::string& source_prefix) override;

    int RemainingCount(const NsId& ns) const;

private:
    mutable std::mutex mu_;
    std::vector<std::pair<NsId, std::string>> blocks_;  // (ns, source_uri)
};

}  // namespace cortrix::import
