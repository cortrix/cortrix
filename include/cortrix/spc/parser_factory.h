#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cortrix/spc/parser.h"

namespace cortrix::spc {

/// DoclingParser config (F06 §2.2). The actual DoclingParser class is implemented in S2; S1 only needs this POD
/// to make ParserFactoryConfig complete.
struct DoclingConfig {
    std::string python_path = "python3";              ///< Python interpreter path
    std::string script_path = "";                     ///< docling_bridge.py path (empty=auto-locate)
    int subprocess_timeout_ms = 60000;                ///< subprocess timeout (should be > ParserOptions)
    int64_t max_output_size_bytes = 100 * 1024 * 1024;///< Max output 100MB
};

/// PaddleOCRParser config (F06 §2.3). The actual class is implemented in S3.
struct PaddleOCRConfig {
    std::string python_path = "python3";
    std::string script_path = "";                     ///< paddleocr_bridge.py path
    int subprocess_timeout_ms = 120000;               ///< OCR is usually slower
    bool use_gpu = false;
    std::string lang = "ch";                          ///< Chinese by default
};

/// Factory + fallback orchestration config (F06 §2.4).
struct ParserFactoryConfig {
    DoclingConfig docling;
    PaddleOCRConfig paddleocr;
    bool enable_paddleocr = true;          ///< Global OCR fallback switch

    /// Average-confidence threshold that triggers fallback: when the primary parser's average confidence is below this value (or
    /// 0 paragraphs / a high failed_pages ratio), fall back to OCR. Default 0.3 (aligned with §4.4.2 ARCH).
    float fallback_confidence_threshold = 0.3f;
};

/// Factory + fallback orchestration for the document parsing framework (F06 §2.4).
///
/// S1 scope: the ParseDocument framework (file pre-checks + extension→MIME inference + parser selection +
/// fallback logic) + RegisterParser/ListParsers + NeedsFallback + MIME mapping.
/// The default Docling/PaddleOCR parsers are injected in S2/S3; in S1, injecting stubs via SetPrimaryParser /
/// SetFallbackParser is enough to test the orchestration logic end-to-end (standalone, no Python dependency).
class DocumentParserFactory {
public:
    explicit DocumentParserFactory(const ParserFactoryConfig& config);

    /// Select a parser based on file extension and config and run parsing (including fallback orchestration).
    /// File pre-checks: not found→FILE_NOT_FOUND / over limit→FILE_TOO_LARGE / unsupported extension→
    /// UNSUPPORTED_FORMAT. Primary parser low-confidence/empty pages and fallback allowed → OCR.
    /// @return ParsedDoc — consumed directly by F34 ChunkerInput.pages + F08 GeneratorInput.doc_metadata
    ParsedDoc ParseDocument(const std::string& filepath,
                            const ParserOptions& opts = ParserOptions{});

    /// Register a custom parser (by name). Used for extension (e.g. an S5+ VLM Parser).
    void RegisterParser(const std::string& name,
                        std::unique_ptr<IDocumentParser> parser);

    /// Get the names of all registered parsers (primary + fallback + custom, deduplicated, in registration order).
    std::vector<std::string> ListParsers() const;

    /// Inject the primary parser (default Docling). S2 injects the real DoclingParser; S1 injects a stub.
    void SetPrimaryParser(std::unique_ptr<IDocumentParser> parser);

    /// Inject the OCR fallback parser (default PaddleOCR). S3 injects the real PaddleOCRParser.
    void SetFallbackParser(std::unique_ptr<IDocumentParser> parser);

    /// Decide whether the primary parse result needs fallback (based on average confidence + paragraph count
    /// + failed_pages ratio). Public so tests can verify the decision logic directly.
    bool NeedsFallback(const ParsedDoc& result, float threshold) const;

    /// Get the MIME type for an extension (lowercase extension, without the dot). Unknown extension returns "".
    static std::string ExtensionToMimeType(const std::string& ext);

    /// Get the lowercase extension (without the dot) from a file path; no extension returns "".
    static std::string ExtractExtension(const std::string& filepath);

private:
    /// Routing: select the primary parser by extension. The current primary parser (Docling) covers all
    /// supported formats except pure images; pure images (png/jpg/tiff) go straight to OCR. Returns nullptr when no parser can handle it.
    IDocumentParser* SelectPrimary(const std::string& ext) const;

    ParserFactoryConfig config_;
    std::unique_ptr<IDocumentParser> primary_;    ///< Docling (default)
    std::unique_ptr<IDocumentParser> fallback_;   ///< PaddleOCR (OCR fallback)
    std::unordered_map<std::string, std::unique_ptr<IDocumentParser>> custom_parsers_;
    std::vector<std::string> custom_order_;       ///< custom registration order (keeps ListParsers stable)
};

}  // namespace cortrix::spc
