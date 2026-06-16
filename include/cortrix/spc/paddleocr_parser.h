#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/spc/parser.h"

namespace cortrix::spc {

/// PaddleOCRParser config (F06 §2.3). Held by value; defaults match the spec.
struct PaddleOCRParserConfig {
    std::string python_path = "python3";
    std::string script_path = "";                     ///< paddleocr_bridge.py path
    int subprocess_timeout_ms = 120000;               ///< OCR is usually slower
    int64_t max_output_size_bytes = 100 * 1024 * 1024;
    bool use_gpu = false;
    std::string lang = "ch";                          ///< Chinese by default
};

/// OCR fallback parser (F06 §2.3): drives PaddleOCR via a Python subprocess
/// (paddleocr_bridge.py) for scanned PDFs and images. Outputs the same §3.1
/// page-level envelope as Docling; OCR paragraphs are all TEXT with empty
/// section (OCR can't distinguish tables/captions). Used by the factory as the
/// fallback when Docling returns empty / low-confidence pages.
///
/// D3 standalone: the wrapper logic is exercised against a *mock* bridge — the
/// machine has python3 but not necessarily paddleocr. Real OCR is D3.5.
class PaddleOCRParser : public IDocumentParser {
public:
    explicit PaddleOCRParser(PaddleOCRParserConfig config);

    ParsedDoc Parse(const std::string& filepath,
                    const ParserOptions& opts = ParserOptions{}) override;
    std::vector<std::string> SupportedFormats() const override;
    const char* Name() const override { return "paddleocr"; }

    /// argv for the bridge subprocess. Public so a test can assert the contract.
    std::vector<std::string> BuildArgv(const std::string& filepath,
                                       const ParserOptions& opts) const;

private:
    PaddleOCRParserConfig config_;
};

}  // namespace cortrix::spc
