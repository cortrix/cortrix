#pragma once
#include <string>

namespace cortrix {

/// Route SPC tasks by block_type and determine processing paths
class SPCRouter {
public:
    /// Infer MIME type from filename extension
    static std::string InferMimeType(const std::string& filename);

    /// Check if MIME type is supported for processing
    static bool IsSupported(const std::string& mime_type);

    /// Determine block_type from MIME type
    static int InferBlockType(const std::string& mime_type);

    /// Check if file needs OCR (image-only PDF, scanned images)
    static bool NeedsOcr(const std::string& mime_type, bool has_text);

    /// Infer processing level from MIME type (D3 routing table)
    /// @return L0=0 (skip), L1=1 (metadata), L2=2 (FTS), L3=3 (full)
    static uint8_t InferProcessingLevel(const std::string& mime_type);
};

}  // namespace cortrix
