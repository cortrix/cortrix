#include <cstdint>
#include "cortrix/spc/docling_parser.h"

#include <utility>

#include "cortrix/spc/parser_subprocess.h"

namespace cortrix::spc {

namespace {

// stderr tail kept in the error body (the full buffer is already capped at 64KB
// by the subprocess runner; this trims it further for the JSON payload).
std::string Tail(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(s.size() - n);
}

}  // namespace

DoclingParser::DoclingParser(DoclingParserConfig config)
    : config_(std::move(config)) {}

std::vector<std::string> DoclingParser::SupportedFormats() const {
    // format table: Docling handles everything except pure images (those go
    // to OCR via the factory).
    return {"pdf", "docx", "pptx", "xlsx", "html", "htm", "md", "markdown", "txt"};
}

std::vector<std::string> DoclingParser::BuildArgv(const std::string& filepath,
                                                  const ParserOptions& opts) const {
    // request line: python3 docling_bridge.py --filepath … --timeout …
    // --max-pages … --language-hint … --output-format json
    std::vector<std::string> argv = {
        config_.python_path,
        config_.script_path,
        "--filepath", filepath,
        "--timeout", std::to_string(opts.subprocess_timeout_ms / 1000),
        "--max-pages", std::to_string(opts.max_pages),
        "--language-hint", opts.language_hint,
        "--output-format", "json",
    };
    return argv;
}

ParsedDoc DoclingParser::Parse(const std::string& filepath,
                               const ParserOptions& opts) {
    // subprocess wall-clock budget = min(parser absolute, configured subprocess)
    // so neither bound is exceeded; both are honored.
    int timeout_ms = config_.subprocess_timeout_ms;
    if (opts.subprocess_timeout_ms > 0 &&
        opts.subprocess_timeout_ms < timeout_ms) {
        timeout_ms = opts.subprocess_timeout_ms;
    }

    const std::vector<std::string> argv = BuildArgv(filepath, opts);
    SubprocessResult sp = RunParserSubprocess(argv, timeout_ms,
                                              config_.max_output_size_bytes);

    // --- map the subprocess outcome to ParserError (steps 3-6) ---
    if (!sp.launched) {
        return MakeErrorDoc(ParserErrorCode::kSubprocessFailed,
                            "Failed to launch Docling bridge subprocess",
                            {{"hint", "Python is not installed or dependencies are missing"}}, Name());
    }
    if (sp.timed_out) {
        return MakeErrorDoc(ParserErrorCode::kParseTimeout, "Subprocess timeout",
                            {{"timeout_ms", timeout_ms},
                             {"pages_completed_before_timeout", -1}}, Name());
    }
    if (sp.output_truncated) {
        return MakeErrorDoc(ParserErrorCode::kOutputTooLarge,
                            "Subprocess output exceeded limit",
                            {{"actual_bytes", static_cast<int64_t>(sp.stdout_data.size())},
                             {"limit_bytes", config_.max_output_size_bytes}}, Name());
    }
    if (sp.exit_code != 0) {
        return MakeErrorDoc(ParserErrorCode::kSubprocessCrashed,
                            "Subprocess exited with non-zero code",
                            {{"exit_code", sp.exit_code},
                             {"stderr_tail", Tail(sp.stderr_data, 1024)}}, Name());
    }

    // --- exit 0: parse the JSON payload (handles bridge-reported errors
    //     and the empty-document OK path internally) ---
    ParsedDoc doc = ParseBridgeJson(sp.stdout_data, filepath, Name());
    DrivePageProgress(doc, opts);  // Q1 page-level progress (standalone replay)
    return doc;
}

}  // namespace cortrix::spc
