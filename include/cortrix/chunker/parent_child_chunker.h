#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/chunker/chunker_config.h"
#include "cortrix/chunker/i_chunker.h"
#include "cortrix/chunker/types.h"
#include "cortrix/common/result.h"

// F34 ParentChildChunker — the Phase 1 V1.0 IChunker implementation (detailed design § 4.1).
//
// Splits a document (F06 ParsedPage[]) into a two-layer chunk tree:
//   - paragraphs are accumulated until the running token count reaches
//     parent_size → flushed as one ParentChunk;
//   - within each parent, the parent_text is recursively split into Children of
//     child_size tokens with child_overlap token overlap;
//   - parent/child IDs are time-ordered ULIDs, metadata is inherited per § 3.2;
//   - when the estimated parent count exceeds max_parents_per_doc, the whole doc
//     degrades to a flat (single-layer) chunking (§ 4.5, D4 lock).
namespace cortrix::chunker {

class ParentChildChunker : public IChunker {
public:
    /// @param config  resolved ChunkerConfig (defaults = § 2.3 locks).
    /// @param token_counter  text → token count. Defaults to EstimateTokens (the
    ///   standalone char-based heuristic); production injects the bge-m3
    ///   HfTokenizer-backed counter (D3.5). Must be deterministic + total.
    explicit ParentChildChunker(ChunkerConfig config = {},
                                TokenCounter token_counter = EstimateTokens);

    /// Chunk one document. Tolerates per-page F06 failures (skips, records in
    /// stats); returns a CX_ERR_CHUNK_* Status only for hard errors:
    ///   - empty document (no usable page)            → CX_ERR_CHUNK_EMPTY_DOCUMENT
    ///   - parent count > cap AND flat fallback failed → CX_ERR_CHUNK_OVERFLOW
    /// (detailed design § 4.1 / § 4.5)
    Result<ChunkerOutput> Chunk(const ChunkerInput& input) override;

    const ChunkerConfig& GetConfig() const override { return config_; }

private:
    // A unit of source text (one paragraph, § 4.1) carrying its provenance so a
    // flushed parent can compute page_start/page_end + byte offsets.
    struct Unit {
        std::string text;
        uint32_t token_count = 0;
        uint32_t page_num = 0;
        uint64_t byte_offset = 0;  // running byte offset within the document
    };

    /// Collect the splitting units (paragraphs for unit_level=paragraph; whole
    /// pages for page; sentence-level otherwise) from the input pages, skipping
    /// F06-failed / empty paragraphs and updating stats (succeeded/failed pages).
    std::vector<Unit> CollectUnits(const ChunkerInput& input, ChunkerStats* stats) const;

    /// Parent-child path: accumulate units into parents, split each parent into
    /// children, assign IDs + inherit metadata. Appends to output.
    void BuildParentChild(const ChunkerInput& input, const std::vector<Unit>& units,
                          int64_t now_ms, ChunkerOutput* output) const;

    /// Flat fallback path (§ 4.5): the whole document becomes one flat layer of
    /// child-sized chunks with no parent-child relationship (each child's
    /// parent_id is empty). Sets stats.fallback_to_flat + appends a warning.
    void BuildFlat(const ChunkerInput& input, const std::vector<Unit>& units,
                   int64_t now_ms, ChunkerOutput* output) const;

    /// Split `parent_text` into child texts of ~child_size tokens with
    /// child_overlap token overlap, recording each child's parent_offset. Returns
    /// (text, parent_offset, token_count) triples.
    struct ChildSpan { std::string text; uint32_t parent_offset; uint32_t token_count; };
    std::vector<ChildSpan> SplitIntoChildren(const std::string& parent_text) const;

    ChunkerConfig config_;
    TokenCounter token_counter_;
};

}  // namespace cortrix::chunker
