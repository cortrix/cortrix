#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "cortrix/common/block_types.h"

namespace cortrix {

/// 128-byte packed C struct — zero-copy reads.
/// F09 evolves the MVP layout: +3 fields (flags_ext / enrichment_source /
/// compression_algo) carved from reserved (44B → 41B), magic "CTX\0" → "CRTX",
/// CRC range extended to cover the new fields + reserved. Size stays 128B.
#pragma pack(push, 1)
struct cortrix_block_header_t {
    /* Identification (8B) */
    uint32_t magic;              // "CRTX" = 0x43525458 (F09 rename, Cortrix brand)
    uint16_t header_version;     // Header format version, Phase 1 = 1
    uint16_t header_size;        // Fixed 128

    /* Type (4B) */
    uint16_t block_type;         // CortrixBlockType enum (1-8)
    uint8_t  processing_level;   // ProcessingLevel (0-4)
    uint8_t  flags;              // BlockFlags bitmask

    /* Payload offsets (24B) — relative to header end (byte 128) */
    uint32_t content_offset;     // offset from header end to content start
    uint32_t content_length;
    uint32_t metadata_offset;    // offset from header end to metadata start
    uint32_t metadata_length;
    uint32_t chain_offset;       // offset from header end to chain start
    uint32_t chain_length;

    /* Data digest (20B) */
    uint32_t total_size;         // header + payload total size
    uint64_t content_hash_high;  // SHA-256 first 8 bytes
    uint64_t content_hash_low;   // SHA-256 last 8 bytes

    /* Processing info (8B) */
    uint32_t embedding_model_id;
    uint16_t vector_dim;
    uint16_t chunk_strategy_id;

    /* Timestamps (16B) */
    uint64_t created_at_us;      // microseconds since epoch
    uint64_t updated_at_us;

    /* Checksum (4B) — offset 80 */
    uint32_t crc32;              // CRC32 of header[0..79] + header[84..127] + payload

    /* Phase 1 fields (3B, offsets 84-86) — carved from reserved */
    uint8_t  flags_ext;          // BlockFlagsExt: has_entities/has_summary/...
    uint8_t  enrichment_source;  // EnrichmentSource: none/docling/paddleocr/llm/vision
    uint8_t  compression_algo;   // 0=none, 1=zstd, 2=lz4 (placeholder, Phase 2)

    /* Reserved (41B) — offsets 87-127 */
    uint8_t  reserved[41];
};
#pragma pack(pop)

static_assert(sizeof(cortrix_block_header_t) == 128,
              "cortrix_block_header_t must be 128 bytes");

constexpr uint32_t kBlockMagic    = 0x43525458;  // on-disk big-endian ASCII "CRTX"
constexpr uint16_t kHeaderVersion = 1;           // Phase 1 = the first real V1
constexpr uint16_t kHeaderSize    = 128;

/// Initialize header (fill magic/version/header_size + zero reserved)
void BlockHeaderInit(cortrix_block_header_t* hdr);

/// Build complete block data: header + content_text + metadata_json + processing_chain.
/// Returns complete BLOB (header 128B + variable-length payload).
///
/// F09 appends 3 optional Phase-1 params (flags_ext / enrichment_source /
/// compression_algo); all default to 0 so existing MVP callers need zero changes.
/// (The struct evolves the MVP layout — the std::string/out-ptr signatures are
/// preserved deliberately; F09 §2.3's vector sketch is reconciled to the MVP
/// signature this Feature is told to evolve, per S1 DoD "MVP callers unchanged".)
std::vector<uint8_t> BlockBuild(
    CortrixBlockType block_type,
    ProcessingLevel level,
    const std::string& content_text,
    const std::string& metadata_json,
    const std::string& processing_chain,
    uint16_t vector_dim = 0,
    uint32_t embedding_model_id = 0,
    uint8_t flags_ext = 0,           // BlockFlagsExt bitmask
    uint8_t enrichment_source = 0,   // EnrichmentSource
    uint8_t compression_algo = 0     // 0=none (Phase 1 placeholder)
);

/// Zero-copy parse header from BLOB (validate magic + version + CRC32).
/// header_version != 1 is rejected (no V1/V2 compat in Phase 1).
/// @param data: BLOB data pointer
/// @param len: BLOB length
/// @param hdr: [out] points to header inside data (zero-copy)
/// @return: true = validation passed
bool BlockParse(const void* data, size_t len, const cortrix_block_header_t** hdr);

/// Compute CRC32 over header[0..79] + header[84..127] + payload (skips the crc32
/// field at offset 80-83; covers the Phase-1 fields + reserved).
uint32_t BlockComputeCrc32(const void* data, size_t len);

/// Format all header fields for diagnostics, e.g.
/// "BlockHeader{v=1, type=FILE, level=L3, flags=0x0F, flags_ext=0x03,
///  enrich=LLM, size=1234, crc=0xABCD1234}".
std::string BlockHeaderToString(const cortrix_block_header_t* hdr);

}  // namespace cortrix
