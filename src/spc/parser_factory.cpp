#include <cstdint>
#include "cortrix/spc/parser_factory.h"

#include <algorithm>
#include <cctype>
#include <sys/stat.h>

namespace cortrix::spc {

namespace {

constexpr int64_t kBytesPerMb = 1024 * 1024;

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Error ParsedDocs use the shared cortrix::spc::MakeErrorDoc (parser.cpp) so the
// registry-fill logic lives in one place.

/// The pure-image formats that route straight to OCR (step 2): Docling does
/// not handle them.
bool IsImageExt(const std::string& ext) {
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tiff" ||
           ext == "tif" || ext == "bmp";
}

}  // namespace

DocumentParserFactory::DocumentParserFactory(const ParserFactoryConfig& config)
    : config_(config) {}

std::string DocumentParserFactory::ExtractExtension(const std::string& filepath) {
    // Last path component, then text after the last dot. A leading-dot file with
    // no other dot (".bashrc") has no extension.
    std::string::size_type slash = filepath.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? filepath
                                                     : filepath.substr(slash + 1);
    std::string::size_type dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return "";
    return ToLower(name.substr(dot + 1));
}

std::string DocumentParserFactory::ExtensionToMimeType(const std::string& ext) {
    const std::string e = ToLower(ext);
    if (e == "pdf")  return "application/pdf";
    if (e == "docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (e == "pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    if (e == "xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (e == "html" || e == "htm") return "text/html";
    if (e == "md" || e == "markdown") return "text/markdown";
    if (e == "txt") return "text/plain";
    if (e == "png")  return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "tiff" || e == "tif") return "image/tiff";
    if (e == "bmp")  return "image/bmp";
    return "";
}

void DocumentParserFactory::SetPrimaryParser(std::unique_ptr<IDocumentParser> parser) {
    primary_ = std::move(parser);
}

void DocumentParserFactory::SetFallbackParser(std::unique_ptr<IDocumentParser> parser) {
    fallback_ = std::move(parser);
}

void DocumentParserFactory::RegisterParser(const std::string& name,
                                           std::unique_ptr<IDocumentParser> parser) {
    if (custom_parsers_.find(name) == custom_parsers_.end()) {
        custom_order_.push_back(name);
    }
    custom_parsers_[name] = std::move(parser);
}

std::vector<std::string> DocumentParserFactory::ListParsers() const {
    std::vector<std::string> names;
    if (primary_) names.push_back(primary_->Name());
    if (fallback_) names.push_back(fallback_->Name());
    for (const std::string& n : custom_order_) {
        names.push_back(n);
    }
    return names;
}

IDocumentParser* DocumentParserFactory::SelectPrimary(const std::string& ext) const {
    // step 2: pure images go straight to OCR; everything else (PDF / Office
    // / markup / text) routes to the primary (Docling) parser. PDF per-page
    // pre-detection (step 3) lives inside the Docling/OCR path itself (S2/S3).
    if (IsImageExt(ext)) {
        return fallback_.get();
    }
    return primary_.get();
}

bool DocumentParserFactory::NeedsFallback(const ParsedDoc& result,
                                          float threshold) const {
    // Never fall back on a hard error the primary already classified as a
    // permanent failure of the *file* (password / corrupted): those don't get
    // better with OCR (step 5).
    if (result.status == ParserErrorCode::kPasswordProtected ||
        result.status == ParserErrorCode::kCorruptedFile) {
        return false;
    }

    // Empty pages → try image extraction + OCR (Q2: image-only PPTX / scanned DOCX).
    if (result.pages.empty()) return true;

    // High proportion of failed pages → OCR may recover the scan pages.
    const size_t total = result.pages.size() + result.failed_pages.size();
    if (total > 0) {
        const double failed_ratio =
            static_cast<double>(result.failed_pages.size()) / static_cast<double>(total);
        if (failed_ratio > 0.5) return true;
    }

    // Low average page confidence across the parsed pages → likely a scan the
    // primary mis-handled.
    double sum = 0.0;
    size_t counted = 0;
    for (const ParsedPage& p : result.pages) {
        sum += p.page_metadata.page_confidence;
        ++counted;
    }
    if (counted > 0) {
        const double avg = sum / static_cast<double>(counted);
        if (avg < static_cast<double>(threshold)) return true;
    }
    return false;
}

ParsedDoc DocumentParserFactory::ParseDocument(const std::string& filepath,
                                               const ParserOptions& opts) {
    // --- 1. file pre-check (step 1) ---
    struct stat st {};
    if (::stat(filepath.c_str(), &st) != 0) {
        return MakeErrorDoc(ParserErrorCode::kFileNotFound,
                         "File not found: " + filepath,
                         {{"filepath", filepath}});
    }

    if (opts.max_file_size_mb > 0) {
        const int64_t limit_bytes =
            static_cast<int64_t>(opts.max_file_size_mb) * kBytesPerMb;
        if (static_cast<int64_t>(st.st_size) > limit_bytes) {
            const int64_t actual_mb = static_cast<int64_t>(st.st_size) / kBytesPerMb;
            return MakeErrorDoc(ParserErrorCode::kFileTooLarge,
                             "File size exceeds limit",
                             {{"actual_size_mb", actual_mb},
                              {"limit_mb", opts.max_file_size_mb},
                              {"tier", "ce"}});
        }
    }

    const std::string ext = ExtractExtension(filepath);
    const std::string mime = ExtensionToMimeType(ext);
    if (mime.empty()) {
        return MakeErrorDoc(ParserErrorCode::kUnsupportedFormat,
                         "Unsupported file format: ." + ext,
                         {{"extension", ext},
                          {"supported_formats",
                           nlohmann::json::array({"pdf", "docx", "pptx", "xlsx",
                                                  "html", "md", "txt", "png",
                                                  "jpg", "tiff"})}});
    }

    // --- 2. parser selection (step 2) ---
    IDocumentParser* primary = SelectPrimary(ext);
    if (primary == nullptr) {
        // No parser registered for this route (e.g. image with no OCR parser
        // injected, or Docling not set). All parsers effectively failed.
        return MakeErrorDoc(ParserErrorCode::kAllParsersFailed,
                         "No parser available for ." + ext,
                         {{"attempted", nlohmann::json::array()}});
    }

    // --- 3. primary parse execution ---
    ParsedDoc result = primary->Parse(filepath, opts);

    // --- 4. fallback orchestration (step 4-5, Q2) ---
    const bool fallback_allowed =
        opts.enable_ocr_fallback && config_.enable_paddleocr &&
        fallback_ != nullptr && fallback_.get() != primary;
    if (fallback_allowed && NeedsFallback(result, config_.fallback_confidence_threshold)) {
        ParsedDoc ocr = fallback_->Parse(filepath, opts);
        if (ocr.ok() && !ocr.pages.empty()) {
            // OCR recovered content. If the primary had also produced pages this
            // is a mixed result; otherwise it's a pure OCR result.
            if (!result.pages.empty()) {
                ocr.parser_name = std::string(primary->Name()) + "+" + fallback_->Name();
            }
            return ocr;
        }
        // Both failed → ALL_PARSERS_FAILED (step 4 double failure).
        if (!result.ok() && !ocr.ok()) {
            return MakeErrorDoc(
                ParserErrorCode::kAllParsersFailed,
                "All parsers failed (primary + OCR fallback)",
                {{"attempted",
                  nlohmann::json::array({primary->Name(), fallback_->Name()})}});
        }
        // Primary produced something usable (non-empty) but tripped the
        // low-confidence threshold; OCR added nothing → keep primary's result.
    }

    return result;
}

}  // namespace cortrix::spc
