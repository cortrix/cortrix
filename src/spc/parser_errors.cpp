#include "cortrix/spc/parser_errors.h"

#include <utility>

namespace cortrix::spc {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as constexpr statics so each
// returns a stable reference. The switch in GetParserErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without
// a row" into a warning, which the project treats as a build failure — the
// registry can't silently drift from the enum.
//
// kOk / kEmptyDocument are not errors (empty doc => status=OK). They carry
// category kPermanent + retryable=false as inert placeholders so GetParserErrorInfo
// stays total; call sites gate on IsParserOk() before building an error body.
constexpr ParserErrorInfo kOk                {"CX_ERR_OK",                  ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kFileNotFound      {"CX_ERR_FILE_NOT_FOUND",      ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kFileTooLarge      {"CX_ERR_FILE_TOO_LARGE",      ErrorCategory::kQuota,     false, std::nullopt};
constexpr ParserErrorInfo kUnsupportedFormat {"CX_ERR_UNSUPPORTED_FORMAT",  ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kParseTimeout      {"CX_ERR_PARSE_TIMEOUT",       ErrorCategory::kTimeout,   true,  1000};
constexpr ParserErrorInfo kSubprocessFailed  {"CX_ERR_SUBPROCESS_FAILED",   ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kSubprocessCrashed {"CX_ERR_SUBPROCESS_CRASHED",  ErrorCategory::kTransient, true,  500};
constexpr ParserErrorInfo kInvalidOutput     {"CX_ERR_INVALID_OUTPUT",      ErrorCategory::kTransient, true,  500};
constexpr ParserErrorInfo kOutputTooLarge    {"CX_ERR_OUTPUT_TOO_LARGE",    ErrorCategory::kQuota,     false, std::nullopt};
constexpr ParserErrorInfo kEmptyDocument     {"CX_ERR_EMPTY_DOCUMENT",      ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kEncodingError     {"CX_ERR_ENCODING_ERROR",      ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kOcrFailed         {"CX_ERR_OCR_FAILED",          ErrorCategory::kTransient, true,  1000};
constexpr ParserErrorInfo kAllParsersFailed  {"CX_ERR_ALL_PARSERS_FAILED",  ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kPasswordProtected {"CX_ERR_PASSWORD_PROTECTED",  ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kCorruptedFile     {"CX_ERR_CORRUPTED_FILE",      ErrorCategory::kPermanent, false, std::nullopt};
constexpr ParserErrorInfo kMaxPagesExceeded  {"CX_ERR_MAX_PAGES_EXCEEDED",  ErrorCategory::kQuota,     false, std::nullopt};

}  // namespace

bool IsParserOk(ParserErrorCode code) {
    return code == ParserErrorCode::kOk ||
           code == ParserErrorCode::kEmptyDocument;
}

const ParserErrorInfo& GetParserErrorInfo(ParserErrorCode code) {
    switch (code) {
        case ParserErrorCode::kOk:                return kOk;
        case ParserErrorCode::kFileNotFound:      return kFileNotFound;
        case ParserErrorCode::kFileTooLarge:      return kFileTooLarge;
        case ParserErrorCode::kUnsupportedFormat: return kUnsupportedFormat;
        case ParserErrorCode::kParseTimeout:      return kParseTimeout;
        case ParserErrorCode::kSubprocessFailed:  return kSubprocessFailed;
        case ParserErrorCode::kSubprocessCrashed: return kSubprocessCrashed;
        case ParserErrorCode::kInvalidOutput:     return kInvalidOutput;
        case ParserErrorCode::kOutputTooLarge:    return kOutputTooLarge;
        case ParserErrorCode::kEmptyDocument:     return kEmptyDocument;
        case ParserErrorCode::kEncodingError:     return kEncodingError;
        case ParserErrorCode::kOcrFailed:         return kOcrFailed;
        case ParserErrorCode::kAllParsersFailed:  return kAllParsersFailed;
        case ParserErrorCode::kPasswordProtected: return kPasswordProtected;
        case ParserErrorCode::kCorruptedFile:     return kCorruptedFile;
        case ParserErrorCode::kMaxPagesExceeded:  return kMaxPagesExceeded;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kOk;
}

const char* ParserErrorCodeString(ParserErrorCode code) {
    return GetParserErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredParserStructuredDataKeys(ParserErrorCode code) {
    // "structured_data (key fields)" column, 1:1. Function-local statics →
    // stable references. The `code` key is always present in the body; the lists
    // below name the *additional* required fields per row.
    static const std::vector<std::string> kEmpty{};
    static const std::vector<std::string> kFilepath{"code", "filepath"};
    static const std::vector<std::string> kFileTooLarge{"code", "actual_size_mb", "limit_mb", "tier"};
    static const std::vector<std::string> kUnsupported{"code", "extension", "supported_formats"};
    static const std::vector<std::string> kTimeout{"code", "timeout_ms", "pages_completed_before_timeout"};
    static const std::vector<std::string> kHint{"code", "hint"};
    static const std::vector<std::string> kCrashed{"code", "exit_code", "stderr_tail"};
    static const std::vector<std::string> kInvalidOut{"code", "parse_error_pos"};
    static const std::vector<std::string> kOutputTooLarge{"code", "actual_bytes", "limit_bytes"};
    static const std::vector<std::string> kEncoding{"code", "detected_encoding"};
    static const std::vector<std::string> kOcr{"code", "failed_pages"};
    static const std::vector<std::string> kAllFailed{"code", "attempted"};
    static const std::vector<std::string> kCorrupted{"code", "magic_bytes_expected", "magic_bytes_actual"};
    static const std::vector<std::string> kMaxPages{"code", "actual_pages", "max_pages", "tier"};

    switch (code) {
        case ParserErrorCode::kOk:                return kEmpty;
        case ParserErrorCode::kEmptyDocument:     return kEmpty;   // status=OK, not treated as an error
        case ParserErrorCode::kFileNotFound:      return kFilepath;
        case ParserErrorCode::kFileTooLarge:      return kFileTooLarge;
        case ParserErrorCode::kUnsupportedFormat: return kUnsupported;
        case ParserErrorCode::kParseTimeout:      return kTimeout;
        case ParserErrorCode::kSubprocessFailed:  return kHint;
        case ParserErrorCode::kSubprocessCrashed: return kCrashed;
        case ParserErrorCode::kInvalidOutput:     return kInvalidOut;
        case ParserErrorCode::kOutputTooLarge:    return kOutputTooLarge;
        case ParserErrorCode::kEncodingError:     return kEncoding;
        case ParserErrorCode::kOcrFailed:         return kOcr;
        case ParserErrorCode::kAllParsersFailed:  return kAllFailed;
        case ParserErrorCode::kPasswordProtected: return kHint;
        case ParserErrorCode::kCorruptedFile:     return kCorrupted;
        case ParserErrorCode::kMaxPagesExceeded:  return kMaxPages;
    }
    return kEmpty;
}

bool HasRequiredParserStructuredData(ParserErrorCode code,
                                     const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) return false;
    for (const std::string& key : RequiredParserStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeParserError(ParserErrorCode code,
                                   nlohmann::json structured_data,
                                   const std::string& message) {
    const ParserErrorInfo& info = GetParserErrorInfo(code);
    // The CX_ERR_* identity also lands inside structured_data (the "code"
    // key) so a consumer reading only the structured payload still recovers it.
    if (structured_data.is_object() && !structured_data.contains("code")) {
        structured_data["code"] = info.cx_code;
    }
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode ParserErrorToStatusCode(ParserErrorCode code) {
    switch (code) {
        case ParserErrorCode::kOk:
        case ParserErrorCode::kEmptyDocument:
            return StatusCode::kOk;
        case ParserErrorCode::kFileNotFound:
            return StatusCode::kNotFound;
        case ParserErrorCode::kUnsupportedFormat:
        case ParserErrorCode::kFileTooLarge:
        case ParserErrorCode::kOutputTooLarge:
        case ParserErrorCode::kMaxPagesExceeded:
        case ParserErrorCode::kEncodingError:
        case ParserErrorCode::kPasswordProtected:
        case ParserErrorCode::kCorruptedFile:
            return StatusCode::kInvalidArgument;
        case ParserErrorCode::kParseTimeout:
        case ParserErrorCode::kSubprocessCrashed:
        case ParserErrorCode::kOcrFailed:
            return StatusCode::kUnavailable;
        case ParserErrorCode::kSubprocessFailed:
        case ParserErrorCode::kInvalidOutput:
        case ParserErrorCode::kAllParsersFailed:
            return StatusCode::kInternal;
    }
    return StatusCode::kInternal;
}

Status ParserStatus(ParserErrorCode code, const std::string& detail) {
    if (IsParserOk(code)) return Status::Ok();
    const char* cx = ParserErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(ParserErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::spc
