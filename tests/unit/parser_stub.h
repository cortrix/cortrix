#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "cortrix/spc/parser.h"

namespace cortrix::spc {
namespace test {

/// Test-only stub IDocumentParser (parser S1 "StubParser"): returns a caller-supplied
/// ParsedDoc and records how many times Parse was called. Lets the factory's
/// pre-check / routing / fallback orchestration be exercised standalone — no
/// Python / Docling / OCR dependency.
class StubParser : public IDocumentParser {
public:
    StubParser(std::string name, std::vector<std::string> formats)
        : name_(std::move(name)), formats_(std::move(formats)) {}

    ParsedDoc Parse(const std::string& filepath,
                    const ParserOptions& opts = ParserOptions{}) override {
        ++call_count_;
        last_filepath_ = filepath;
        if (on_parse_) on_parse_(filepath, opts);
        ParsedDoc doc = result_;
        // Stamp parser_name on success so callers can assert which parser ran,
        // mirroring real parsers that set Name() into the result.
        if (doc.ok() && doc.parser_name.empty()) doc.parser_name = name_;
        return doc;
    }

    std::vector<std::string> SupportedFormats() const override { return formats_; }
    const char* Name() const override { return name_.c_str(); }

    // --- test knobs ---
    void SetResult(ParsedDoc result) { result_ = std::move(result); }
    void SetOnParse(std::function<void(const std::string&, const ParserOptions&)> cb) {
        on_parse_ = std::move(cb);
    }
    int call_count() const { return call_count_.load(); }
    const std::string& last_filepath() const { return last_filepath_; }

private:
    std::string name_;
    std::vector<std::string> formats_;
    ParsedDoc result_;
    std::function<void(const std::string&, const ParserOptions&)> on_parse_;
    // atomic: async task WorkerPool tests drive Parse from pool workers while the
    // test thread polls call_count() for completion.
    std::atomic<int> call_count_{0};
    std::string last_filepath_;
};

/// Build a minimal successful single-page ParsedDoc with the given confidence —
/// convenient for factory fallback tests.
inline ParsedDoc MakeOnePageDoc(const std::string& parser_name,
                                float page_confidence,
                                const std::string& text = "hello") {
    ParsedDoc doc;
    doc.status = ParserErrorCode::kOk;
    doc.parser_name = parser_name;
    ParsedChunk para;
    para.text = text;
    para.page = 1;
    para.type = ChunkType::TEXT;
    para.confidence = page_confidence;
    ParsedPage page;
    page.page_num = 1;
    page.page_text = text;
    page.paragraphs.push_back(para);
    page.page_metadata.page_num = 1;
    page.page_metadata.parser_used = parser_name;
    page.page_metadata.page_confidence = page_confidence;
    page.page_metadata.char_count = static_cast<int>(text.size());
    doc.pages.push_back(page);
    doc.metadata.page_count = 1;
    return doc;
}

}  // namespace test
}  // namespace cortrix::spc
