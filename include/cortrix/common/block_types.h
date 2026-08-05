#pragma once
#include <cstdint>

namespace cortrix {

/// Block sub-type enum (matches DDL block_type column).
/// F09 is the SoT for this enum — downstream Features (F38 kBlockHypeQuestion=16,
/// F41 doc-summary, ...) extend it here, not via private values.
enum CortrixBlockType : uint16_t {
    kBlockFile     = 1,
    kBlockScan     = 2,   // OCR
    kBlockDatabase = 3,   // CDC
    kBlockAudio    = 4,
    kBlockVideo    = 5,
    kBlockImage    = 6,
    kBlockMemory   = 7,
    kBlockMeta     = 8,   // F09/Phase 1: document-metadata block
    kBlockHypeQuestion = 16,  // F38: HyPE hypothetical-question block (F09 §4.4 authorized)
    kBlockDocSummary   = 17,  // F41: document-level LLM summary block (F09 §4.4 authorized; F08-4: independent, not via F08)
};

/// Block processing level (unified INTEGER across pipeline)
enum ProcessingLevel : uint8_t {
    kLevelL0 = 0,   // Skip (no processing)
    kLevelL1 = 1,   // Metadata only
    kLevelL2 = 2,   // BM25 + metadata
    kLevelL3 = 3,   // Vector + BM25 + metadata (fully processed)
    kLevelL4 = 4,   // F09/Phase 1: LLM Enhanced (NER + Summary + Vision)
};

/// Header flags bitmask (MVP — unchanged by F09)
enum BlockFlags : uint8_t {
    kFlagHasContent  = 0x01,  // bit0
    kFlagHasVector   = 0x02,  // bit1
    kFlagHasFts      = 0x04,  // bit2
    kFlagHasMetadata = 0x08,  // bit3
    kFlagHasChain    = 0x10,  // bit4
    kFlagCompressed  = 0x20,  // bit5: payload compressed (placeholder, Phase 2)
    // bit6-7: reserved
};

/// Extended flags (flags_ext field, F09/Phase 1). F09 is the SINGLE SoT for the
/// flags_ext bit assignment (V14 J1, 2026-05-29): downstream Features
/// MUST #include this enum and not allocate bits privately
/// (this whole-bitmap lock fixed the prior bit0/bit1 double-occupation).
enum BlockFlagsExt : uint8_t {
    kFlagExtHasEntities       = 0x01,  // bit0: payload has NER entities JSON
    kFlagExtHasSummary        = 0x02,  // bit1: payload has summary text
    kFlagExtIsAnomalous       = 0x04,  // bit2: F10 cleaning marked this block anomalous
    kFlagExtHasContextualized = 0x08,  // bit3: F35 contextualized embedding
    kFlagExtHasSparseVec      = 0x10,  // bit4: F40 sparse vector
    // bit5-7 (0x20/0x40/0x80): reserved (encryption-related, Phase 2)
};

/// Enrichment source that produced the block's payload (enrichment_source field).
enum EnrichmentSource : uint8_t {
    kEnrichNone      = 0,
    kEnrichDocling   = 1,   // Docling structured parse
    kEnrichPaddleOCR = 2,   // PaddleOCR text extraction
    kEnrichLlm       = 3,   // LLM NER/Summary
    kEnrichVision    = 4,   // LLM Vision description
};

}  // namespace cortrix
