#pragma once
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/import/import_types.h"
#include "cortrix/import/spc_feed.h"

namespace cortrix {
class SPCManager;
namespace resource { class INamespacePool; }
}  // namespace cortrix

namespace cortrix::import {

/// [D3.5 r2 · Wave P · P2b] Real ISpcFeeder over the live SPCManager — replaces
/// InMemorySpcFeeder (feed_to_spc_pipeline, step 6 "feed the
/// textualized rows into the Pipeline"). Each TextChunk (one textualized DB row,
/// or a MERGE batch) becomes a single document: a one-page ParsedDoc carrying the
/// chunk text + the §4.3 source metadata, handed to SPCManager::ProcessParsedDoc
/// (the same post-parse seam DocumentProcessor uses — Chunk→META→enrich→embed→
/// assemble→coordinated write). doc_id is a fresh ULID; content_hash is the chunk text
/// hash (the dedup identity). The resulting blocks are real + searchable.
///
/// Borrows the SPCManager (must outlive this feeder). 0 change to SPCManager /
/// SPCPipeline — this only *calls* the frozen ProcessParsedDoc surface.
class SpcManagerFeeder : public ISpcFeeder {
public:
    explicit SpcManagerFeeder(SPCManager* spc_mgr) : spc_mgr_(spc_mgr) {}

    /// Feed each chunk as a document through the pipeline into namespace `ns`.
    /// Returns the count successfully ingested (== chunks.size() on full success);
    /// a per-chunk pipeline failure aborts with a CX_ERR_IMPORT_* Status.
    Result<int> Feed(const std::vector<TextChunk>& chunks, const NsId& ns) override;

private:
    SPCManager* spc_mgr_;
};

/// [D3.5 r2 · Wave P · P2c] Real IBlockCleaner for the D3 full-overwrite step
/// (cleanup_source_blocks). Acquires the target namespace's façade
/// (the same per-Unit store the SpcManagerFeeder writes into) and deletes every
/// document whose source_path begins with `source_prefix` together with its blocks,
/// via CortrixStore::doc_delete_by_source_prefix (one txn; FTS cleaned by trigger).
/// Returns the count of blocks removed. A re-import thus clears the prior table's
/// rows before the new ones land (no duplicate rows). Borrows the namespace pool (must
/// outlive this cleaner).
class RealBlockCleaner : public IBlockCleaner {
public:
    explicit RealBlockCleaner(resource::INamespacePool* pool) : pool_(pool) {}

    Result<int> CleanupSourceBlocks(const NsId& ns,
                                    const std::string& source_prefix) override;

private:
    resource::INamespacePool* pool_;
};

/// [D3.5 r2 · Wave P · P2b] No-op IBlockCleaner — retained as the standalone/test
/// double (never touches a store). Production uses RealBlockCleaner.
class NoopBlockCleaner : public IBlockCleaner {
public:
    Result<int> CleanupSourceBlocks(const NsId& ns,
                                    const std::string& source_prefix) override;
};

}  // namespace cortrix::import
