#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cortrix/spc/parser_errors.h"

namespace cortrix::spc {

/// Parse options. Config sources: global_config → NS override → request-level override (catalog merge).
/// NS-overridable: max_pages / max_file_size_mb / enable_ocr_fallback / language_hint /
/// subprocess_timeout_ms. Not NS-overridable (system resource level): parser_max_concurrent /
/// parser_default_concurrent (Q11 — actual concurrency control at the Pipeline layer).
struct ParserOptions {
    bool enable_ocr_fallback = true;       ///< PDF pre-detection auto-routing + non-PDF empty-pages fallback
    int max_pages = 200;                   ///< CE default 200, Ent default 2000, 0=unlimited (Q10)
    int max_file_size_mb = 200;            ///< CE default 200MB, Ent default 500MB (Q10)
    std::string language_hint = "";        ///< Language hint (empty=auto-detect)
    int max_retries = 2;                   ///< Max retries (including the first attempt; Q1 ruling 3→2)
    int subprocess_timeout_ms = 300000;    ///< Absolute per-document timeout (CE 5 min sync, Ent 30 min async)

    /// Page-level progress callback (Q1). Called once per completed page: (current page, total pages, whether the page succeeded).
    /// nullptr = no callback. (S1 only defines it; per-page driving is in S3.)
    std::function<void(int page, int total, bool success)> on_page_progress = nullptr;
};

/// Chunk type. Serialized as the uppercase string of the JSON protocol ("TEXT" etc.).
enum class ChunkType : uint8_t {
    TEXT = 0,
    TABLE = 1,
    IMAGE_CAPTION = 2,
    CODE = 3,
    META = 4,
};

const char* ToString(ChunkType type);
/// Reverse: JSON string → ChunkType (unknown string falls back to TEXT). Used by bridge JSON parsing (S2).
ChunkType ChunkTypeFromString(const std::string& s);

/// Parsed paragraph-level text chunk (ParsedPage.paragraphs[] sub-structure).
/// content_hash is computed by the chunker after the final chunk is formed, not in parser output.
struct ParsedChunk {
    std::string text;          ///< Text content (UTF-8)
    int page = -1;             ///< Page number (1-based, -1 means not applicable)
    std::string section;       ///< Section heading (empty string = no section info)
    ChunkType type = ChunkType::TEXT;
    float confidence = 0.0f;   ///< Parse confidence [0.0, 1.0]
    std::string language;      ///< Detected language (ISO 639-1, e.g. "zh", "en")
    int char_offset = -1;      ///< Character offset in the source document (CE fixed at -1; Ent ParsedChunkExt has a value)
    int char_length = -1;      ///< Character length
};

/// Page-level metadata.
struct PageMetadata {
    int page_num = 0;              ///< Page number (1-based)
    std::string parser_used;       ///< Parser used for this page ("docling" / "paddleocr")
    float page_confidence = 0.0f;  ///< Average confidence over the whole page
    bool is_scan_page = false;     ///< Whether this is a scanned page (PDF pre-detection result)
    int char_count = 0;            ///< Character count of the whole page
};

/// Single-page parse output. Maps 1:1 to ChunkerInput.pages;
/// paragraphs[] is the input material for parent-child splitting.
struct ParsedPage {
    int page_num = 0;                       ///< Page number (1-based)
    std::string page_text;                  ///< Whole-page concatenated text (merged in paragraphs order)
    std::vector<ParsedChunk> paragraphs;    ///< Paragraph-level sub-chunks
    PageMetadata page_metadata;             ///< Page-level metadata
};

/// Document metadata. DocumentMetadata is the single SoT —— enricher consumers
/// reference this struct, do not create another. doc_language = document-level language (distinct from the
/// paragraph-level ParsedChunk.language). section_heading is paragraph-level (ParsedChunk.section), not in this struct.
struct DocumentMetadata {
    std::string filename;          ///< Original file name
    std::string doc_title;         ///< Document title (empty=no title, falls back to filename) — {{doc_title}}
    std::string mime_type;         ///< MIME type (the old enricher note "type" refers to this field)
    int64_t file_size_bytes = 0;   ///< File size
    int page_count = -1;           ///< Total page count (-1=not applicable)
    std::string doc_language;      ///< Primary document language — {{doc_language}} / consumed by the enricher
    int64_t upload_timestamp = 0;  ///< Upload timestamp (Unix epoch ms) — {{upload_timestamp}}
    int64_t parse_time_ms = 0;     ///< Parse duration
};

/// Parse result (ParseResult → ParsedDoc). Consumed directly by the downstream
/// ChunkerInput.pages + GeneratorInput.doc_metadata.
///
/// An error response (status != 0 and not EMPTY_DOCUMENT) carries the 4 fields of the Agent-friendly 7 principles
/// (retryable / category / retry_after_ms / structured_data_json). Use
/// MakeAgentFriendlyError() to obtain the canonical agent_friendly::AgentFriendlyError, serialized
/// uniformly at the API/SDK/MCP boundary (the Agent-friendly contract). See + parser_errors.h.
struct ParsedDoc {
    ParserErrorCode status = ParserErrorCode::kOk;  ///< kOk / kEmptyDocument = success
    std::vector<ParsedPage> pages;          ///< Page-level output (v1.0.1 replaces chunks)
    DocumentMetadata metadata;              ///< Document metadata (MetadataGenerator is responsible for generating the Block)
    std::string parser_name;                ///< "docling" / "paddleocr" / "docling+paddleocr"
    std::vector<int> failed_pages;          ///< Page numbers that failed to parse (Q1 per-page fault tolerance)

    // Error response fields (valid when status is an error code) — GEN-Agent 7-principles mandatory fields.
    std::string error_msg;                  ///< Error message (free text, for humans)
    bool retryable = false;                 ///< Principle 6: whether retryable (machine-readable)
    agent_friendly::ErrorCategory category =
        agent_friendly::ErrorCategory::kPermanent;  ///< Principle 4: error category enum
    int retry_after_ms = 0;                 ///< Principle 6: suggested retry wait (required when retryable)
    nlohmann::json structured_data;         ///< Principle 5: structured error data

    /// Whether successful (status == kOk || kEmptyDocument).
    bool ok() const { return IsParserOk(status); }
};

/// Rebuild the canonical Agent-friendly boundary error from a ParsedDoc's error fields. Only meaningful when !ok();
/// a successful result returns a kOk placeholder error. The API/SDK/MCP boundary serializes it with agent_friendly::ToJson().
agent_friendly::AgentFriendlyError MakeAgentFriendlyError(const ParsedDoc& doc);

/// Construct a failed ParsedDoc: fill the Agent-friendly fields (retryable /
/// category / retry_after_ms) from the registry + inject structured_data["code"]. The unified error
/// exit for parsers/factories. parser_name marks the parser that produced the error.
ParsedDoc MakeErrorDoc(ParserErrorCode code, const std::string& message,
                       nlohmann::json structured_data = nlohmann::json::object(),
                       const std::string& parser_name = "");

/// Parse the bridge subprocess's stdout JSON (page-level protocol) into a ParsedDoc.
/// Tolerates missing fields (uses struct defaults); invalid JSON / non-object top level → INVALID_OUTPUT.
/// An error reported by the bridge itself (status != 0 and non-empty) is also respected and mapped to the corresponding ParserErrorCode.
/// @param json_str  subprocess stdout
/// @param filepath  original file path (fills metadata.filename as a fallback)
/// @param parser_name caller's parser name ("docling"/"paddleocr"), used as a fallback when the bridge does not provide one
ParsedDoc ParseBridgeJson(const std::string& json_str,
                          const std::string& filepath,
                          const std::string& parser_name);

/// If opts.on_page_progress is non-null, drive the callback per page (Q1 page-level progress): call once for each
/// successful page of doc (success=true) and each failed_pages page (success=false); total =
/// successful pages + failed pages; callbacks fire in ascending page-number order. The standalone implementation = a single
/// replay after parsing completes (true per-page streaming driving requires incremental bridge output → integration). No callback when doc is an error result.
void DrivePageProgress(const ParsedDoc& doc, const ParserOptions& opts);

/// Abstract document parser interface. Pluggable: DoclingParser (default) / PaddleOCRParser
/// (OCR fallback) / custom implementations (VLM etc. from S5 on) all implement this interface and register with the Factory.
class IDocumentParser {
public:
    virtual ~IDocumentParser() = default;

    /// Parse a document file.
    /// @param filepath temporary file path (local path after upload)
    /// @param opts parse options
    /// @return ParsedDoc — consumed directly by downstream ChunkerInput.pages
    virtual ParsedDoc Parse(const std::string& filepath,
                            const ParserOptions& opts = ParserOptions{}) = 0;

    /// List of supported file extensions (lowercase, without the dot, e.g. {"pdf","docx"}).
    virtual std::vector<std::string> SupportedFormats() const = 0;

    /// Parser name ("docling" / "paddleocr" / ...).
    virtual const char* Name() const = 0;

    /// Check whether a given extension is supported (case-insensitive, tolerates a leading dot). Defaults to
    /// being based on SupportedFormats(); concrete parsers usually need not override it.
    virtual bool IsFormatSupported(const std::string& extension) const;
};

}  // namespace cortrix::spc
