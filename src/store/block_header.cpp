#include <cstdint>
#include "cortrix/common/block_header.h"
#include <chrono>
#include <cstdio>
#include <zlib.h>
#include <openssl/evp.h>

namespace cortrix {

void BlockHeaderInit(cortrix_block_header_t* hdr) {
    std::memset(hdr, 0, sizeof(cortrix_block_header_t));
    hdr->magic = kBlockMagic;
    hdr->header_version = kHeaderVersion;
    hdr->header_size = kHeaderSize;
}

std::vector<uint8_t> BlockBuild(
    CortrixBlockType block_type,
    ProcessingLevel level,
    const std::string& content_text,
    const std::string& metadata_json,
    const std::string& processing_chain,
    uint16_t vector_dim,
    uint32_t embedding_model_id,
    uint8_t flags_ext,
    uint8_t enrichment_source,
    uint8_t compression_algo)
{
    // Calculate payload sizes
    uint32_t payload_offset = kHeaderSize;
    uint32_t content_len = static_cast<uint32_t>(content_text.size());
    uint32_t metadata_len = static_cast<uint32_t>(metadata_json.size());
    uint32_t chain_len = static_cast<uint32_t>(processing_chain.size());
    uint32_t total_size = kHeaderSize + content_len + metadata_len + chain_len;

    // Allocate buffer
    std::vector<uint8_t> buf(total_size, 0);

    // Build header
    auto* hdr = reinterpret_cast<cortrix_block_header_t*>(buf.data());
    BlockHeaderInit(hdr);

    hdr->block_type = static_cast<uint16_t>(block_type);
    hdr->processing_level = static_cast<uint8_t>(level);

    // Set flags
    uint8_t flags = 0;
    if (content_len > 0) flags |= kFlagHasContent;
    if (metadata_len > 0) flags |= kFlagHasMetadata;
    if (chain_len > 0) flags |= kFlagHasChain;
    if (vector_dim > 0) flags |= kFlagHasVector;
    // FTS5 flag: set for L2/L3 blocks that have content text
    // (these will be indexed by the FTS5 trigger on INSERT)
    if (content_len > 0 && (level == ProcessingLevel::kLevelL2 || level == ProcessingLevel::kLevelL3)) {
        flags |= kFlagHasFts;
    }
    hdr->flags = flags;

    // Payload offsets — relative to header end (byte 128)
    uint32_t abs_offset = payload_offset;  // absolute position in buffer
    if (content_len > 0) {
        hdr->content_offset = abs_offset - kHeaderSize;  // relative to header end
        hdr->content_length = content_len;
        std::memcpy(buf.data() + abs_offset, content_text.data(), content_len);
        abs_offset += content_len;
    }
    if (metadata_len > 0) {
        hdr->metadata_offset = abs_offset - kHeaderSize;  // relative to header end
        hdr->metadata_length = metadata_len;
        std::memcpy(buf.data() + abs_offset, metadata_json.data(), metadata_len);
        abs_offset += metadata_len;
    }
    if (chain_len > 0) {
        hdr->chain_offset = abs_offset - kHeaderSize;  // relative to header end
        hdr->chain_length = chain_len;
        std::memcpy(buf.data() + abs_offset, processing_chain.data(), chain_len);
        abs_offset += chain_len;
    }

    hdr->total_size = total_size;
    hdr->vector_dim = vector_dim;
    hdr->embedding_model_id = embedding_model_id;

    // Compute content hash: SHA-256 of content_text, store first 16 bytes
    // as content_hash_high (bytes 0-7) and content_hash_low (bytes 8-15)
    if (content_len > 0) {
        unsigned char sha256_digest[32] = {};
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (ctx) {
            if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                EVP_DigestUpdate(ctx, content_text.data(), content_text.size()) == 1) {
                unsigned int digest_len = 0;
                EVP_DigestFinal_ex(ctx, sha256_digest, &digest_len);
            }
            EVP_MD_CTX_free(ctx);
        }
        std::memcpy(&hdr->content_hash_high, sha256_digest, 8);
        std::memcpy(&hdr->content_hash_low, sha256_digest + 8, 8);
    }

    // Timestamps
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    hdr->created_at_us = static_cast<uint64_t>(us);
    hdr->updated_at_us = static_cast<uint64_t>(us);

    // Phase 1 fields (offsets 84-86). Set BEFORE CRC — the CRC now covers them.
    hdr->flags_ext = flags_ext;
    hdr->enrichment_source = enrichment_source;
    hdr->compression_algo = compression_algo;

    // CRC32 (header[0..79] + header[84..127] + payload, skipping crc32 field)
    hdr->crc32 = BlockComputeCrc32(buf.data(), total_size);

    return buf;
}

bool BlockParse(const void* data, size_t len, const cortrix_block_header_t** hdr) {
    if (!data || len < kHeaderSize || !hdr) {
        return false;
    }

    const auto* h = reinterpret_cast<const cortrix_block_header_t*>(data);

    // Validate magic
    if (h->magic != kBlockMagic) {
        return false;
    }

    // Validate header version
    if (h->header_version != kHeaderVersion) {
        return false;
    }

    // Validate total_size
    if (h->total_size > len) {
        return false;
    }

    // Validate CRC32
    uint32_t expected_crc = BlockComputeCrc32(data, h->total_size);
    if (expected_crc != h->crc32) {
        return false;
    }

    *hdr = h;
    return true;
}

uint32_t BlockComputeCrc32(const void* data, size_t len) {
    // CRC32 covers: header[0..79] + header[84..127] + payload (offset 128+).
    // Skips only the crc32 field itself (offset 80-83). F09 extends the MVP
    // range to include the Phase-1 fields (84-86) + reserved (87-127).
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);

    uLong crc = crc32(0L, Z_NULL, 0);

    // Header bytes [0..79] (before crc32 field)
    if (len >= 80) {
        crc = crc32(crc, bytes, 80);
    }

    // Header bytes [84..127] (after crc32 field, incl. Phase-1 fields + reserved)
    if (len >= kHeaderSize) {
        crc = crc32(crc, bytes + 84, kHeaderSize - 84);
    }

    // Payload bytes [128..total_size)
    if (len > kHeaderSize) {
        crc = crc32(crc, bytes + kHeaderSize, static_cast<uInt>(len - kHeaderSize));
    }

    return static_cast<uint32_t>(crc);
}

namespace {

const char* BlockTypeName(uint16_t t) {
    switch (t) {
        case kBlockFile:     return "FILE";
        case kBlockScan:     return "SCAN";
        case kBlockDatabase: return "DATABASE";
        case kBlockAudio:    return "AUDIO";
        case kBlockVideo:    return "VIDEO";
        case kBlockImage:    return "IMAGE";
        case kBlockMemory:   return "MEMORY";
        case kBlockMeta:     return "META";
        default:             return "UNKNOWN";
    }
}

const char* EnrichName(uint8_t e) {
    switch (e) {
        case kEnrichNone:      return "none";
        case kEnrichDocling:   return "Docling";
        case kEnrichPaddleOCR: return "PaddleOCR";
        case kEnrichLlm:       return "LLM";
        case kEnrichVision:    return "Vision";
        default:               return "unknown";
    }
}

}  // namespace

std::string BlockHeaderToString(const cortrix_block_header_t* hdr) {
    if (hdr == nullptr) return "BlockHeader{null}";
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "BlockHeader{v=%u, type=%s, level=L%u, flags=0x%02X, flags_ext=0x%02X, "
        "enrich=%s, compress=%u, size=%u, crc=0x%08X}",
        static_cast<unsigned>(hdr->header_version),
        BlockTypeName(hdr->block_type),
        static_cast<unsigned>(hdr->processing_level),
        static_cast<unsigned>(hdr->flags),
        static_cast<unsigned>(hdr->flags_ext),
        EnrichName(hdr->enrichment_source),
        static_cast<unsigned>(hdr->compression_algo),
        static_cast<unsigned>(hdr->total_size),
        static_cast<unsigned>(hdr->crc32));
    return std::string(buf);
}

}  // namespace cortrix
