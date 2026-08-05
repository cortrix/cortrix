#include <cstdint>
#include "cortrix/spc/block_assembler.h"

#include "cortrix/id/hash.h"   // id::HashChildIdToBlockId
#include "cortrix/id/ulid.h"   // id::GenerateUlid

namespace cortrix {

CortrixBlock BlockAssembler::Assemble(const std::string& doc_id,
                                     const ChunkResult& chunk,
                                     const EmbeddingResult& embedding,
                                     CortrixBlockType block_type,
                                     uint8_t processing_level,
                                     const std::string& metadata_json) {
    CortrixBlock block;
    block.doc_id = doc_id;
    block.chunk_index = chunk.chunk_index;
    block.block_type = static_cast<int>(block_type);
    block.processing_level = static_cast<int>(processing_level);
    block.content_text = chunk.text;
    // block_id = HashChildIdToBlockId(child_id) (ID_SYSTEM_IMPL §6.1 / ARCH §1.8):
    // mint the chunk's business ULID (its child_id) here and derive the uint64
    // P-HNSW label from its SipHash. The parent-child chunker will supply the
    // child_id later; the block_id formula is unchanged, so this is the final form.
    block.block_id = id::HashChildIdToBlockId(id::GenerateUlid());

    // Build the binary block data (header 128B + payload)
    auto level = static_cast<ProcessingLevel>(processing_level);
    block.data = BlockBuild(
        block_type,
        level,
        chunk.text,
        metadata_json,
        "",  // processing_chain (empty for MVP)
        static_cast<uint16_t>(embedding.dim),
        0    // embedding_model_id
    );

    return block;
}

CortrixBlock BlockAssembler::AssembleChild(const cortrix::chunker::ChildChunk& child,
                                          const EmbeddingResult& embedding,
                                          CortrixBlockType modality,
                                          uint8_t processing_level,
                                          const std::string& metadata_json,
                                          uint8_t flags_ext) {
    CortrixBlock block;
    block.doc_id = child.doc_id;
    block.chunk_index = static_cast<int>(child.chunk_index);
    // Candidate B: a child's block_type is the document's source modality
    // (kBlockFile/...), NOT a kBlockChild enum; child-ness is carried by child_id.
    block.block_type = static_cast<int>(modality);
    block.processing_level = static_cast<int>(processing_level);
    block.content_text = child.child_text;
    // block_id = HashChildIdToBlockId(real child ULID) — the P-HNSW label (ARCH §1.8.2).
    block.block_id = id::HashChildIdToBlockId(child.child_id);
    // A child columns carried onto the blocks row.
    block.child_id = child.child_id;
    block.parent_id = child.parent_id;
    block.token_count = static_cast<int64_t>(child.token_count);
    block.parent_offset = static_cast<int64_t>(child.parent_offset);
    block.metadata_json = metadata_json;
    // Binary block payload (header 128B + payload), same shape as the flat path.
    auto level = static_cast<ProcessingLevel>(processing_level);
    block.data = BlockBuild(
        modality, level, child.child_text, metadata_json, "",
        static_cast<uint16_t>(embedding.dim), 0, flags_ext);
    return block;
}

}  // namespace cortrix
