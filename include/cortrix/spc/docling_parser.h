#pragma once
#include <string>
#include <vector>

#include "cortrix/spc/parser.h"

namespace cortrix::spc {

/// DoclingParser config (F06 §2.2). Held by value; defaults match the spec.
struct DoclingParserConfig {
    std::string python_path = "python3";              ///< Python interpreter path
    std::string script_path = "";                     ///< docling_bridge.py path (empty=auto-locate)
    int subprocess_timeout_ms = 60000;                ///< subprocess timeout (should be > ParserOptions)
    int64_t max_output_size_bytes = 100 * 1024 * 1024;///< Max output 100MB
};

/// Default document parser (F06 §2.2): drives the Docling library via a Python
/// subprocess (docling_bridge.py) and maps its JSON output (§3.1 page-level
/// protocol) to ParsedDoc.
///
/// D3 standalone: the wrapper logic (command build / timeout / output cap /
/// error-code mapping / JSON→ParsedDoc) is what's exercised here, against a
/// *mock* bridge script — the machine has python3 but not necessarily docling.
/// Real docling end-to-end belongs to D3.5.
class DoclingParser : public IDocumentParser {
public:
    explicit DoclingParser(DoclingParserConfig config);

    ParsedDoc Parse(const std::string& filepath,
                    const ParserOptions& opts = ParserOptions{}) override;
    std::vector<std::string> SupportedFormats() const override;
    const char* Name() const override { return "docling"; }

    /// argv for the bridge subprocess (§3.1 request line). Public so a test can
    /// assert the request contract without spawning a process.
    std::vector<std::string> BuildArgv(const std::string& filepath,
                                       const ParserOptions& opts) const;

private:
    DoclingParserConfig config_;
};

}  // namespace cortrix::spc
