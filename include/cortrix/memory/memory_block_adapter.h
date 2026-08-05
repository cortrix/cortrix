#pragma once
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/memory/memory_extractor.h"      // IMemoryBlockStore / MemoryBlockRecord
#include "cortrix/memory/memory_transparency.h"   // IMemoryBlockLister

namespace cortrix {
class CortrixStore;
class OnnxEmbedder;
namespace store { class IIndex; }
}  // namespace cortrix

namespace cortrix::memory {

// Store-backed adapters used by the MEM02 MemoryExtractor
// + MEM03 MemoryTransparency depend on through their seams (IMemoryBlockStore /
// IMemoryBlockLister). Memory facts are rows of the per-Unit `blocks` table
// (block_type = kBlockMemory) whose metadata_json carries the MEM02 §4.1/§4.2 fields
// (memory_type / status / user_id / provenance / invalidation state). The adapter is
// NS-scoped — constructed over one namespace's CortrixStore (+ embedder / vec index
// so a freshly extracted memory is searchable). block_id (the MEM02 ULID) maps to the
// uint64 blocks.block_id via HashChildIdToBlockId.
//
// Queries that filter on metadata_json (ListByUser, FindCandidates, the user_facts
// read) run direct SQL over the store's sqlite handle using SQLite's json_extract; a
// store with no real handle (test fakes) degrades to an empty result.
class MemoryBlockAdapter : public IMemoryBlockStore,
                           public transparency::IMemoryBlockLister {
public:
    /// @param store     the per-NS block store (block_insert/get + sqlite handle).
    /// @param embedder  embeds memory content so the fact is semantically searchable
    ///                  (nullptr → memory rows are stored without a vector).
    /// @param vec_index the per-NS P-HNSW index the memory vector is added to
    ///                  (nullptr → no vector indexing).
    MemoryBlockAdapter(CortrixStore& store, OnnxEmbedder* embedder,
                       store::IIndex* vec_index);

    // --- IMemoryBlockStore ---
    Result<std::string> InsertMemoryBlock(const MemoryBlockRecord& block) override;
    Status UpdateMemoryBlock(const MemoryBlockRecord& block) override;
    Result<MemoryBlockRecord> GetMemoryBlock(const std::string& block_id) override;

    // --- IMemoryBlockLister ---
    Result<std::vector<MemoryBlockRecord>> ListByUser(const std::string& user_id) override;

private:
    CortrixStore& store_;
    OnnxEmbedder* embedder_;
    store::IIndex* vec_index_;
};

/// Contradiction-retrieval seam (D5) over the per-NS memory blocks. Standalone MEM02
/// used a mock; this is the real adapter. It runs a semantic-ish nearest-memory
/// retrieval: for the contradiction judge it is sufficient to surface the user's
/// existing same-type memories (the LLM judge then decides actual contradiction), so
/// the adapter returns the active fact/preference blocks for the namespace ordered by
/// recency (a full vector RRF over memories is a Phase-2 refinement). user-scope
/// isolation is applied by the caller (the extractor passes the new memory's user_id
/// into ns; here we filter active memory blocks).
class MemoryContradictionAdapter : public IContradictionQuery {
public:
    explicit MemoryContradictionAdapter(CortrixStore& store);

    std::vector<CandidateBlock> FindCandidates(const std::string& ns,
                                               const std::string& content,
                                               int top_k) override;
    std::optional<CandidateBlock> FindMatchingPreference(
        const std::string& ns, const std::string& content) override;

private:
    CortrixStore& store_;
};

/// One user-fact row returned to F39's chat path (the #22 stable interface Wave Q
/// consumes). A flat projection of an active memory block: id + content + type +
/// confidence, newest first. No vector search — chat memory injection wants the
/// user's durable facts/preferences, not a query-relevance ranking.
struct UserFact {
    std::string block_id;
    std::string content;
    std::string memory_type;   ///< "fact" / "preference" / "event"
    double confidence = 0.0;
    std::string created_at;    ///< ISO 8601
};

/// Query a user's active memory facts in one namespace (F39 chat path / #22). Returns
/// the active (status=active), non-expired memory blocks owned by `user_id`, newest
/// first, capped at `limit`. A null store handle (test fake) → empty. This is the
/// stable seam Wave Q's F39 chat router binds to; its shape is frozen here so the
/// query side and the memory side can be merged independently.
Result<std::vector<UserFact>> QueryUserFacts(CortrixStore& store,
                                             const std::string& user_id, int limit);

}  // namespace cortrix::memory
