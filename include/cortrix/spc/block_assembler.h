#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "cortrix/common/data_types.h"
#include "cortrix/common/block_header.h"
#include "cortrix/chunker/types.h"   // [A unified-blocks] ChildChunk
#include "cortrix/spc/recursive_chunker.h"
#include "cortrix/spc/onnx_embedder.h"

namespace cortrix {

/// Assembles CortrixBlock from chunk results and embeddings
class BlockAssembler {
public:
    /// Assemble a single block from chunk + embedding (MVP flat path; block_id is a
    /// fresh hashed ULID).
    CortrixBlock Assemble(const std::string& doc_id,
                         const ChunkResult& chunk,
                         const EmbeddingResult& embedding,
                         CortrixBlockType block_type,
                         uint8_t processing_level = 3,
                         const std::string& metadata_json = "");

    /// [A unified-blocks] Assemble a child block (F34 ParentChildChunker output) as a
    /// row of the unified `blocks` table. block_id = HashChildIdToBlockId(child.child_id)
    /// (the real child ULID — the P-HNSW label); block_type = the document's source
    /// `modality` (candidate B — child-ness is carried by child_id, no kBlockChild);
    /// child_id/parent_id/token_count/parent_offset are set on the block; metadata_json
    /// is the inherited per-child metadata (caller-serialized from child.metadata).
    /// `flags_ext` is the F09 BlockFlagsExt bitmask baked into the header (default 0
    /// = no flags; existing callers unchanged). The SPC pipeline passes
    /// kFlagExtHasContextualized (bit3, F35 §4.1) when the child carries an F35
    /// contextualized embedding so the read path can branch on the header without a
    /// column read.
    CortrixBlock AssembleChild(const cortrix::chunker::ChildChunk& child,
                               const EmbeddingResult& embedding,
                               CortrixBlockType modality,
                               uint8_t processing_level = 3,
                               const std::string& metadata_json = "",
                               uint8_t flags_ext = 0);
};

}  // namespace cortrix
