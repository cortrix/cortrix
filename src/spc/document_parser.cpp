#include "cortrix/spc/document_parser.h"
#include "cortrix/spc/subprocess_utils.h"
#include "cortrix/config/config.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cortrix {

// --- PdfParser ---

PdfParser::PdfParser(const std::string& python_bin,
                     const std::string& script_path,
                     int timeout_s)
    : python_bin_(python_bin), script_path_(script_path), timeout_s_(timeout_s) {}

Status PdfParser::Parse(const std::string& file_path,
                        const std::string& mime_type,
                        ParseResult* result) {
    (void)mime_type;
    if (!result) return Status::InvalidArgument("null result pointer");

    // Call Python subprocess: python3 parse_pdf.py <file_path>
    // Use argv-based execution to prevent shell injection via file paths
    std::vector<std::string> args = {python_bin_, script_path_, file_path};
    std::string output;
    Status s = RunSubprocess(args, timeout_s_, &output);
    if (!s.ok()) {
        return Status::Internal("PDF parse failed: " + s.message());
    }

    // Parse JSON output
    try {
        json j = json::parse(output);

        // Check for error in JSON
        if (j.contains("error")) {
            return Status::Internal("PDF parse error: " + j["error"].get<std::string>());
        }

        result->text = j.value("text", "");
        result->title = j.value("title", "");
        result->page_count = j.value("page_count", 0);
        result->needs_ocr = j.value("needs_ocr", false);
        result->char_count = static_cast<int64_t>(result->text.size());
        result->language = "unknown";

    } catch (const json::exception& e) {
        return Status::Internal("Failed to parse PDF JSON output: " + std::string(e.what()));
    }

    return Status::Ok();
}

bool PdfParser::Supports(const std::string& mime_type) const {
    return mime_type == "application/pdf";
}

// --- WordParser ---

WordParser::WordParser(const std::string& python_bin,
                       const std::string& script_path,
                       int timeout_s)
    : python_bin_(python_bin), script_path_(script_path), timeout_s_(timeout_s) {}

Status WordParser::Parse(const std::string& file_path,
                         const std::string& mime_type,
                         ParseResult* result) {
    (void)mime_type;
    if (!result) return Status::InvalidArgument("null result pointer");

    // Use argv-based execution to prevent shell injection via file paths
    std::vector<std::string> args = {python_bin_, script_path_, file_path};
    std::string output;
    Status s = RunSubprocess(args, timeout_s_, &output);
    if (!s.ok()) {
        return Status::Internal("Word parse failed: " + s.message());
    }

    // Parse JSON output
    try {
        json j = json::parse(output);

        // Check for error in JSON
        if (j.contains("error")) {
            return Status::Internal("Word parse error: " + j["error"].get<std::string>());
        }

        result->text = j.value("text", "");
        result->title = j.value("title", "");
        result->page_count = j.value("page_count", 0);
        result->char_count = static_cast<int64_t>(result->text.size());
        result->language = "unknown";

    } catch (const json::exception& e) {
        return Status::Internal("Failed to parse Word JSON output: " + std::string(e.what()));
    }

    return Status::Ok();
}

bool WordParser::Supports(const std::string& mime_type) const {
    return mime_type == "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
}

// --- MarkdownParser ---

Status MarkdownParser::Parse(const std::string& file_path,
                             const std::string& mime_type,
                             ParseResult* result) {
    (void)mime_type;
    if (!result) return Status::InvalidArgument("null result pointer");

    std::ifstream file(file_path);
    if (!file.is_open()) {
        return Status::NotFound("cannot open file: " + file_path);
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    // Strip Markdown formatting to plain text
    // Note: Use multiline flag for ^ and $ to match line starts/ends
    const auto multiline = std::regex_constants::ECMAScript | std::regex_constants::multiline;

    // Remove headers (# ## ### etc.)
    std::string text = std::regex_replace(content, std::regex("^#{1,6}\\s+", multiline), "");
    // Remove bold/italic markers
    text = std::regex_replace(text, std::regex("\\*{1,3}([^*]+)\\*{1,3}"), "$1");
    text = std::regex_replace(text, std::regex("_{1,3}([^_]+)_{1,3}"), "$1");
    // Remove inline code
    text = std::regex_replace(text, std::regex("`([^`]+)`"), "$1");
    // Remove links [text](url) -> text
    text = std::regex_replace(text, std::regex("\\[([^\\]]+)\\]\\([^)]+\\)"), "$1");
    // Remove images ![alt](url)
    text = std::regex_replace(text, std::regex("!\\[([^\\]]*)\\]\\([^)]+\\)"), "$1");
    // Remove horizontal rules
    text = std::regex_replace(text, std::regex("^[-*_]{3,}$", multiline), "");
    // Remove blockquote markers
    text = std::regex_replace(text, std::regex("^>\\s?", multiline), "");

    result->text = text;
    result->char_count = static_cast<int64_t>(text.size());
    result->language = "unknown";
    return Status::Ok();
}

bool MarkdownParser::Supports(const std::string& mime_type) const {
    return mime_type == "text/markdown";
}

// --- TxtParser ---

Status TxtParser::Parse(const std::string& file_path,
                        const std::string& mime_type,
                        ParseResult* result) {
    (void)mime_type;
    if (!result) return Status::InvalidArgument("null result pointer");

    std::ifstream file(file_path);
    if (!file.is_open()) {
        return Status::NotFound("cannot open file: " + file_path);
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    result->text = ss.str();
    result->char_count = static_cast<int64_t>(result->text.size());
    result->language = "unknown";
    return Status::Ok();
}

bool TxtParser::Supports(const std::string& mime_type) const {
    return mime_type == "text/plain";
}

// --- DocumentParserFactory ---

DocumentParserFactory::DocumentParserFactory(const SPCConfig& config) {
    pdf_parser_ = std::make_unique<PdfParser>(
        config.python_bin, config.parse_pdf_script, config.parser_timeout_s);
    word_parser_ = std::make_unique<WordParser>(
        config.python_bin, config.parse_word_script, config.parser_timeout_s);
    md_parser_ = std::make_unique<MarkdownParser>();
    txt_parser_ = std::make_unique<TxtParser>();
}

DocumentParser* DocumentParserFactory::GetParser(const std::string& mime_type) {
    if (pdf_parser_->Supports(mime_type)) return pdf_parser_.get();
    if (word_parser_->Supports(mime_type)) return word_parser_.get();
    if (md_parser_->Supports(mime_type)) return md_parser_.get();
    if (txt_parser_->Supports(mime_type)) return txt_parser_.get();
    return nullptr;
}

}  // namespace cortrix
