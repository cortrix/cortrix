#include <cstdint>
#include "cortrix/spc/spc_router.h"
#include "cortrix/common/block_types.h"
#include <algorithm>

namespace cortrix {

std::string SPCRouter::InferMimeType(const std::string& filename) {
    // Find extension
    auto dot_pos = filename.rfind('.');
    if (dot_pos == std::string::npos) {
        return "application/octet-stream";
    }
    std::string ext = filename.substr(dot_pos);
    // Lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Document types (routing table)
    if (ext == ".pdf")  return "application/pdf";
    if (ext == ".docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ext == ".doc")  return "application/msword";
    if (ext == ".xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (ext == ".xls")  return "application/vnd.ms-excel";
    if (ext == ".pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    if (ext == ".md" || ext == ".markdown") return "text/markdown";
    if (ext == ".txt")  return "text/plain";
    if (ext == ".csv")  return "text/csv";
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".json") return "application/json";
    if (ext == ".xml")  return "application/xml";

    // Image types
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".tiff" || ext == ".tif")  return "image/tiff";
    if (ext == ".bmp")  return "image/bmp";

    // L0 skip types (temp/binary/font files)
    if (ext == ".tmp" || ext == ".bak") return "application/x-temp";
    if (ext == ".exe" || ext == ".dll" || ext == ".so") return "application/x-executable";
    if (ext == ".ttf" || ext == ".otf" || ext == ".woff") return "font/ttf";

    return "application/octet-stream";
}

bool SPCRouter::IsSupported(const std::string& mime_type) {
    // routing table: all these types are processable
    return mime_type == "application/pdf" ||
           mime_type == "application/vnd.openxmlformats-officedocument.wordprocessingml.document" ||
           mime_type == "application/msword" ||
           mime_type == "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" ||
           mime_type == "application/vnd.ms-excel" ||
           mime_type == "application/vnd.openxmlformats-officedocument.presentationml.presentation" ||
           mime_type == "text/markdown" ||
           mime_type == "text/plain" ||
           mime_type == "text/csv" ||
           mime_type == "text/html" ||
           mime_type == "application/json" ||
           mime_type == "application/xml" ||
           mime_type == "text/xml" ||
           mime_type == "image/png" ||
           mime_type == "image/jpeg" ||
           mime_type == "image/tiff" ||
           mime_type == "image/bmp";
}

int SPCRouter::InferBlockType(const std::string& mime_type) {
    if (mime_type == "image/png" || mime_type == "image/jpeg" ||
        mime_type == "image/tiff" || mime_type == "image/bmp") {
        return kBlockScan;
    }
    return kBlockFile;
}

bool SPCRouter::NeedsOcr(const std::string& mime_type, bool has_text) {
    // Images always need OCR
    if (mime_type == "image/png" || mime_type == "image/jpeg" ||
        mime_type == "image/tiff" || mime_type == "image/bmp") {
        return true;
    }
    // PDF with no text needs OCR (image-only PDF) — PROBE mechanism
    if (mime_type == "application/pdf" && !has_text) {
        return true;
    }
    return false;
}

uint8_t SPCRouter::InferProcessingLevel(const std::string& mime_type) {
    // routing table: determine processing level
    // L0: temp files, executables, fonts, empty/unknown binary
    if (mime_type == "application/x-temp" ||
        mime_type == "application/x-executable" ||
        mime_type == "font/ttf") {
        return 0;  // L0: skip
    }

    // L3: most document types get full processing
    // (PDF, Word, Excel, text, markdown, CSV, HTML, JSON, XML, images)
    return 3;  // L3: full processing
}

}  // namespace cortrix
