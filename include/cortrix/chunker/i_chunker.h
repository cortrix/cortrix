#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cortrix/chunker/chunker_config.h"
#include "cortrix/chunker/types.h"
#include "cortrix/common/result.h"
#include "cortrix/id/types.h"
#include "cortrix/spc/parser.h"  // cortrix::spc::ParsedPage / DocumentMetadata

// F34 IChunker — the L1 chunking abstraction (detailed design § 2.1).
//
// Phase 1 V1.0 has a single implementation (ParentChildChunker); the interface
// is kept abstract so Phase 2 can add semantic / fixed-size / hybrid chunkers.
namespace cortrix::chunker {

/// A token counter: text → token count. Pluggable so the chunker stays decoupled
/// from any specific tokenizer (production wires the bge-m3 HfTokenizer; tests use
/// a deterministic char-based estimate). Must be deterministic and total.
using TokenCounter = std::function<uint32_t(const std::string&)>;

/// Default standalone token estimate when no real tokenizer is wired (D3.5 wires
/// HfTokenizer). Heuristic mirrors RecursiveChunker's char-based approximation:
/// ASCII bytes count ~1/4 token (≈4 chars/token), each non-ASCII UTF-8 lead byte
/// counts as ~1 token (CJK ≈ 1 token/char). Never returns 0 for non-empty text.
uint32_t EstimateTokens(const std::string& text);

/// Chunker input — one document's F06 parse output + doc-level metadata (§ 2.1).
struct ChunkerInput {
    cortrix::id::DocId       doc_id;
    cortrix::id::NamespaceId namespace_id;
    std::vector<cortrix::spc::ParsedPage> pages;  ///< F06 Docling output
    DocumentMetadata metadata;                    ///< doc-level (inherited to parent/child)
};

/// Chunker output — flat parents + children (relationship via child.parent_id /
/// parent.child_ids) + stats (§ 2.1).
struct ChunkerOutput {
    std::vector<ParentChunk> parents;
    std::vector<ChildChunk>  children;
    ChunkerStats stats;
};

/// L1 chunking abstraction. `Chunk` is fallible (Result<ChunkerOutput>): hard
/// errors (empty document / overflow) come back as a chunker Status carrying a
/// CX_ERR_CHUNK_* code (chunker_errors.h); non-fatal page failures are tolerated
/// and recorded in output.stats (§ 4.5).
class IChunker {
public:
    virtual ~IChunker() = default;
    virtual Result<ChunkerOutput> Chunk(const ChunkerInput& input) = 0;
    virtual const ChunkerConfig& GetConfig() const = 0;
};

}  // namespace cortrix::chunker
