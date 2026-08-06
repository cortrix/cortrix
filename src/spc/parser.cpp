#include <cstdint>
#include "cortrix/spc/parser.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace cortrix::spc {

namespace {

// The bridge's status int is the wire form of ParserError; we trust the
// registry for category/retryability rather than the JSON "category" string, so
// only the numeric status is mapped here.
ParserErrorCode StatusToCode(int status) {
    if (status < 0 || status > static_cast<int>(ParserErrorCode::kMaxPagesExceeded)) {
        return ParserErrorCode::kInvalidOutput;  // out-of-range → treat as bad output
    }
    return static_cast<ParserErrorCode>(status);
}

}  // namespace

const char* ToString(ChunkType type) {
    switch (type) {
        case ChunkType::TEXT:          return "TEXT";
        case ChunkType::TABLE:         return "TABLE";
        case ChunkType::IMAGE_CAPTION: return "IMAGE_CAPTION";
        case ChunkType::CODE:          return "CODE";
        case ChunkType::META:          return "META";
    }
    return "TEXT";
}

ChunkType ChunkTypeFromString(const std::string& s) {
    if (s == "TABLE") return ChunkType::TABLE;
    if (s == "IMAGE_CAPTION") return ChunkType::IMAGE_CAPTION;
    if (s == "CODE") return ChunkType::CODE;
    if (s == "META") return ChunkType::META;
    return ChunkType::TEXT;  // default / fallback for an unknown string
}

agent_friendly::AgentFriendlyError MakeAgentFriendlyError(const ParsedDoc& doc) {
    // Rebuild the canonical body from the registry (single source of truth for
    // code / category / retryability), preserving the per-instance structured_data
    // and human message the parser attached.
    nlohmann::json sd = doc.structured_data.is_object()
                            ? doc.structured_data
                            : nlohmann::json::object();
    return MakeParserError(doc.status, std::move(sd), doc.error_msg);
}

ParsedDoc MakeErrorDoc(ParserErrorCode code, const std::string& message,
                       nlohmann::json structured_data,
                       const std::string& parser_name) {
    const ParserErrorInfo& info = GetParserErrorInfo(code);
    if (structured_data.is_object() && !structured_data.contains("code")) {
        structured_data["code"] = info.cx_code;
    }
    ParsedDoc doc;
    doc.status = code;
    doc.parser_name = parser_name;
    doc.error_msg = message;
    doc.retryable = info.retryable;
    doc.category = info.category;
    doc.retry_after_ms = info.retry_after_ms.value_or(0);
    doc.structured_data = std::move(structured_data);
    return doc;
}

namespace {

ParsedChunk ParseChunk(const nlohmann::json& j) {
    ParsedChunk c;
    c.text = j.value("text", "");
    c.page = j.value("page", -1);
    c.section = j.value("section", "");
    c.type = ChunkTypeFromString(j.value("type", "TEXT"));
    c.confidence = j.value("confidence", 0.0f);
    c.language = j.value("language", "");
    c.char_offset = j.value("char_offset", -1);
    c.char_length = j.value("char_length", -1);
    return c;
}

PageMetadata ParsePageMeta(const nlohmann::json& j, int page_num) {
    PageMetadata m;
    m.page_num = j.value("page_num", page_num);
    m.parser_used = j.value("parser_used", "");
    m.page_confidence = j.value("page_confidence", 0.0f);
    m.is_scan_page = j.value("is_scan_page", false);
    m.char_count = j.value("char_count", 0);
    return m;
}

DocumentMetadata ParseDocMeta(const nlohmann::json& j, const std::string& filepath) {
    DocumentMetadata m;
    m.filename = j.value("filename", "");
    if (m.filename.empty()) {
        // Fall back to the basename of the input path.
        auto slash = filepath.find_last_of("/\\");
        m.filename = (slash == std::string::npos) ? filepath : filepath.substr(slash + 1);
    }
    m.doc_title = j.value("doc_title", "");
    m.mime_type = j.value("mime_type", "");
    m.file_size_bytes = j.value("file_size_bytes", static_cast<int64_t>(0));
    m.page_count = j.value("page_count", -1);
    m.doc_language = j.value("doc_language", "");
    m.upload_timestamp = j.value("upload_timestamp", static_cast<int64_t>(0));
    m.parse_time_ms = j.value("parse_time_ms", static_cast<int64_t>(0));
    return m;
}

}  // namespace

ParsedDoc ParseBridgeJson(const std::string& json_str,
                          const std::string& filepath,
                          const std::string& parser_name) {
    nlohmann::json j = nlohmann::json::parse(json_str, /*cb=*/nullptr,
                                             /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return MakeErrorDoc(ParserErrorCode::kInvalidOutput,
                            "Bridge output is not valid JSON object",
                            {{"parse_error_pos", -1}}, parser_name);
    }

    const int status_int = j.value("status", 0);
    const ParserErrorCode code = StatusToCode(status_int);

    // Bridge reported an error (and it's not the benign empty-doc OK path).
    if (!IsParserOk(code)) {
        nlohmann::json sd = j.value("structured_data", nlohmann::json::object());
        if (!sd.is_object()) sd = nlohmann::json::object();
        const std::string msg = j.value("error_msg", "");
        return MakeErrorDoc(code, msg, std::move(sd),
                            j.value("parser", parser_name));
    }

    // Success (incl. empty document, status=OK + pages=[]).
    ParsedDoc doc;
    doc.status = code;
    doc.parser_name = j.value("parser", parser_name);
    doc.metadata = ParseDocMeta(j.value("metadata", nlohmann::json::object()), filepath);

    if (j.contains("pages") && j["pages"].is_array()) {
        for (const auto& pj : j["pages"]) {
            ParsedPage page;
            page.page_num = pj.value("page_num", 0);
            page.page_text = pj.value("page_text", "");
            page.page_metadata = ParsePageMeta(
                pj.value("page_metadata", nlohmann::json::object()), page.page_num);
            if (pj.contains("paragraphs") && pj["paragraphs"].is_array()) {
                for (const auto& cj : pj["paragraphs"]) {
                    page.paragraphs.push_back(ParseChunk(cj));
                }
            }
            doc.pages.push_back(std::move(page));
        }
    }

    if (j.contains("failed_pages") && j["failed_pages"].is_array()) {
        for (const auto& fp : j["failed_pages"]) {
            if (fp.is_number_integer()) doc.failed_pages.push_back(fp.get<int>());
        }
    }
    return doc;
}

void DrivePageProgress(const ParsedDoc& doc, const ParserOptions& opts) {
    if (!opts.on_page_progress) return;
    if (!doc.ok()) return;  // a doc-level failure has no per-page progress

    // Merge succeeded + failed page numbers, ordered ascending, and report each
    // once with its success flag. total = succeeded + failed (Q1).
    std::vector<std::pair<int, bool>> events;  // (page_num, success)
    events.reserve(doc.pages.size() + doc.failed_pages.size());
    for (const ParsedPage& p : doc.pages) events.emplace_back(p.page_num, true);
    for (int fp : doc.failed_pages) events.emplace_back(fp, false);
    std::sort(events.begin(), events.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const int total = static_cast<int>(events.size());
    for (const auto& [page_num, success] : events) {
        opts.on_page_progress(page_num, total, success);
    }
}

bool IDocumentParser::IsFormatSupported(const std::string& extension) const {
    // Normalize: drop a leading dot, lowercase. Cheap + tolerant of caller input
    // ("PDF", ".pdf", "pdf" all match).
    std::string ext = extension;
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const std::string& fmt : SupportedFormats()) {
        if (fmt == ext) return true;
    }
    return false;
}

}  // namespace cortrix::spc
