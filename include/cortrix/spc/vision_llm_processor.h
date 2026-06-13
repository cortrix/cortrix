#pragma once
#include <string>
#include "cortrix/common/status.h"
#include "cortrix/spc/ocr_processor.h"
#include "cortrix/config/config.h"

namespace cortrix {

/// Vision LLM processor: sends PDF page images + OCR text to a Vision LLM
/// for enhanced text extraction (OCR+LLM hybrid mode).
class VisionLlmProcessor {
public:
    VisionLlmProcessor(const std::string& python_bin,
                       const std::string& script_path,
                       const LlmConfig& llm_config,
                       int timeout_s = 120);

    /// Process PDF with OCR text as reference (OCR+LLM hybrid).
    /// @param file_path: path to the PDF file
    /// @param ocr_text: OCR-extracted text (pages separated by \f)
    /// @param result: output OcrResult (reuses same format for compatibility)
    Status Process(const std::string& file_path,
                   const std::string& ocr_text,
                   OcrResult* result);

    /// Whether vision LLM is configured and enabled.
    bool IsEnabled() const;

private:
    std::string python_bin_;
    std::string script_path_;
    LlmConfig llm_config_;
    int timeout_s_;
    bool enabled_;
};

}  // namespace cortrix
