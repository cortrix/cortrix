#pragma once
#include <string>
#include <memory>
#include "cortrix/common/status.h"

namespace cortrix {

struct SPCConfig;

struct ParseResult {
    std::string text;
    std::string title;
    std::string language;
    int64_t char_count = 0;
    int page_count = 0;
    bool needs_ocr = false;
    std::string metadata_json;
};

/// Document parser interface
class DocumentParser {
public:
    virtual ~DocumentParser() = default;

    virtual Status Parse(const std::string& file_path,
                         const std::string& mime_type,
                         ParseResult* result) = 0;

    virtual bool Supports(const std::string& mime_type) const = 0;
};

class PdfParser : public DocumentParser {
public:
    PdfParser(const std::string& python_bin, const std::string& script_path, int timeout_s);
    Status Parse(const std::string& file_path, const std::string& mime_type, ParseResult* result) override;
    bool Supports(const std::string& mime_type) const override;
private:
    std::string python_bin_, script_path_;
    int timeout_s_;
};

class WordParser : public DocumentParser {
public:
    WordParser(const std::string& python_bin, const std::string& script_path, int timeout_s);
    Status Parse(const std::string& file_path, const std::string& mime_type, ParseResult* result) override;
    bool Supports(const std::string& mime_type) const override;
private:
    std::string python_bin_, script_path_;
    int timeout_s_;
};

class MarkdownParser : public DocumentParser {
public:
    Status Parse(const std::string& file_path, const std::string& mime_type, ParseResult* result) override;
    bool Supports(const std::string& mime_type) const override;
};

class TxtParser : public DocumentParser {
public:
    Status Parse(const std::string& file_path, const std::string& mime_type, ParseResult* result) override;
    bool Supports(const std::string& mime_type) const override;
};

/// Factory: select parser by MIME type
class DocumentParserFactory {
public:
    explicit DocumentParserFactory(const SPCConfig& config);
    DocumentParser* GetParser(const std::string& mime_type);
private:
    std::unique_ptr<PdfParser> pdf_parser_;
    std::unique_ptr<WordParser> word_parser_;
    std::unique_ptr<MarkdownParser> md_parser_;
    std::unique_ptr<TxtParser> txt_parser_;
};

}  // namespace cortrix
