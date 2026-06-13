#include "cortrix/spc/paddleocr_parser.h"

#include <utility>

#include "cortrix/spc/parser_subprocess.h"

namespace cortrix::spc {

namespace {

std::string Tail(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(s.size() - n);
}

// §4.3: PaddleOCR only handles PDF + image formats; everything else is rejected
// up front (the factory routes Office/markup to Docling, never here).
bool OcrSupports(const std::string& ext) {
    return ext == "pdf" || ext == "png" || ext == "jpg" || ext == "jpeg" ||
           ext == "tiff" || ext == "tif" || ext == "bmp";
}

std::string LowerExt(const std::string& filepath) {
    auto slash = filepath.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? filepath : filepath.substr(slash + 1);
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return "";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

}  // namespace

PaddleOCRParser::PaddleOCRParser(PaddleOCRParserConfig config)
    : config_(std::move(config)) {}

std::vector<std::string> PaddleOCRParser::SupportedFormats() const {
    return {"pdf", "png", "jpg", "jpeg", "tiff", "tif", "bmp"};
}

std::vector<std::string> PaddleOCRParser::BuildArgv(const std::string& filepath,
                                                    const ParserOptions& opts) const {
    std::vector<std::string> argv = {
        config_.python_path,
        config_.script_path,
        "--filepath", filepath,
        "--timeout", std::to_string(opts.subprocess_timeout_ms / 1000),
        "--max-pages", std::to_string(opts.max_pages),
        "--lang", config_.lang,
        "--output-format", "json",
    };
    if (config_.use_gpu) argv.push_back("--use-gpu");
    return argv;
}

ParsedDoc PaddleOCRParser::Parse(const std::string& filepath,
                                 const ParserOptions& opts) {
    const std::string ext = LowerExt(filepath);
    if (!OcrSupports(ext)) {
        return MakeErrorDoc(ParserErrorCode::kUnsupportedFormat,
                            "PaddleOCR cannot process ." + ext,
                            {{"extension", ext},
                             {"supported_formats",
                              nlohmann::json::array({"pdf", "png", "jpg",
                                                     "tiff", "bmp"})}},
                            Name());
    }

    int timeout_ms = config_.subprocess_timeout_ms;
    if (opts.subprocess_timeout_ms > 0 &&
        opts.subprocess_timeout_ms < timeout_ms) {
        timeout_ms = opts.subprocess_timeout_ms;
    }

    const std::vector<std::string> argv = BuildArgv(filepath, opts);
    SubprocessResult sp = RunParserSubprocess(argv, timeout_ms,
                                              config_.max_output_size_bytes);

    if (!sp.launched) {
        return MakeErrorDoc(ParserErrorCode::kSubprocessFailed,
                            "Failed to launch PaddleOCR bridge subprocess",
                            {{"hint", "Python is not installed or the paddleocr dependency is missing"}}, Name());
    }
    if (sp.timed_out) {
        return MakeErrorDoc(ParserErrorCode::kParseTimeout, "OCR subprocess timeout",
                            {{"timeout_ms", timeout_ms},
                             {"pages_completed_before_timeout", -1}}, Name());
    }
    if (sp.output_truncated) {
        return MakeErrorDoc(ParserErrorCode::kOutputTooLarge,
                            "OCR subprocess output exceeded limit",
                            {{"actual_bytes", static_cast<int64_t>(sp.stdout_data.size())},
                             {"limit_bytes", config_.max_output_size_bytes}}, Name());
    }
    if (sp.exit_code != 0) {
        // A crash in the OCR engine maps to OCR_FAILED (retryable, §5.2) rather
        // than the generic SUBPROCESS_CRASHED — for the Agent the actionable
        // signal is "OCR failed, retry".
        return MakeErrorDoc(ParserErrorCode::kOcrFailed,
                            "PaddleOCR subprocess crashed",
                            {{"failed_pages", nlohmann::json::array()},
                             {"exit_code", sp.exit_code},
                             {"stderr_tail", Tail(sp.stderr_data, 1024)}}, Name());
    }

    ParsedDoc doc = ParseBridgeJson(sp.stdout_data, filepath, Name());
    DrivePageProgress(doc, opts);  // Q1 page-level progress (standalone replay)
    return doc;
}

}  // namespace cortrix::spc
