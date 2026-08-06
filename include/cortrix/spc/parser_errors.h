#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::spc {

/// The 16 document-parser error identities. Each maps to a stable
/// `CX_ERR_*` string + a GEN-Agent category + retryability via the canonical
/// registry below.
///
/// Per the coding conventions, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// So ParserErrorCode is the *enum of identities*, and MakeParserError() turns
/// one (plus optional structured_data) into that boundary error — mirroring
/// catalog::CatalogErrorCode (the canonical template).
///
/// The integer values match the ParserError enum so they double as the
/// ParsedDoc.status wire value the Python bridge returns over JSON.
///
/// EMPTY_DOCUMENT (9) is *not* an error: an all-blank document returns
/// status=OK, pages=[]. It is kept in the enum for the bridge's
/// status field but carries category NONE — see GetParserErrorInfo.
enum class ParserErrorCode : uint8_t {
    kOk = 0,
    kFileNotFound = 1,        // File does not exist
    kFileTooLarge = 2,        // Exceeds the max_file_size limit
    kUnsupportedFormat = 3,   // Unsupported file format
    kParseTimeout = 4,        // Parse timeout
    kSubprocessFailed = 5,    // Python subprocess failed to start / communicate
    kSubprocessCrashed = 6,   // Python subprocess crashed (non-zero exit code)
    kInvalidOutput = 7,       // Invalid subprocess output (JSON parse failed)
    kOutputTooLarge = 8,      // Subprocess output exceeds max_output_size
    kEmptyDocument = 9,       // Empty document (not treated as an error, see above)
    kEncodingError = 10,      // Encoding error (non-UTF-8 and conversion failed)
    kOcrFailed = 11,          // PaddleOCR failed
    kAllParsersFailed = 12,   // All parsers (including fallback) failed
    kPasswordProtected = 13,  // Encrypted / password-protected document
    kCorruptedFile = 14,      // Corrupted file
    kMaxPagesExceeded = 15,   // Exceeds the max page-count limit
};

/// Total number of parser error codes (= 16). Compile-time anchor for
/// the API-compatibility regression test (the set must not shrink).
constexpr int kParserErrorCodeCount = 16;

/// Canonical, immutable attributes of one error code (columns). Mirrors
/// catalog::CatalogErrorInfo so the two registries read identically.
struct ParserErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per
};

/// True iff `code` denotes a successful (non-error) outcome. kOk and
/// kEmptyDocument both yield status=OK responses (empty doc is not an
/// error); every other code is an error to be surfaced as an AgentFriendlyError.
bool IsParserOk(ParserErrorCode code);

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the rows.
const ParserErrorInfo& GetParserErrorInfo(ParserErrorCode code);

/// The "CX_ERR_*" string for `code` (convenience over GetParserErrorInfo).
const char* ParserErrorCodeString(ParserErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry
/// ("structured_data" column). The SoT for the Agent-friendly contract
/// (GEN-Agent #5); lets call sites + tests verify the body is complete.
/// kOk / kEmptyDocument return an empty list.
const std::vector<std::string>& RequiredParserStructuredDataKeys(ParserErrorCode code);

/// True iff `structured_data` contains every required key for `code`. Used to
/// assert Agent-friendly completeness before returning an error body.
bool HasRequiredParserStructuredData(ParserErrorCode code,
                                     const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the required keys are the caller's responsibility at each call site) and
/// an optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry — call sites never restate them.
/// Precondition: `code` is an error code (!IsParserOk).
agent_friendly::AgentFriendlyError MakeParserError(
    ParserErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge a parser error to a plain Status for the Result<T>/Status interface
/// surface (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the
/// message is prefixed with the CX_ERR_* token ("CX_ERR_X: detail") so the exact
/// parser identity is recoverable at the API/SDK boundary, which re-inflates the
/// full Agent-friendly body via MakeParserError(). Same pattern as
/// catalog::CatalogStatus — we do NOT widen cortrix::Status itself.
Status ParserStatus(ParserErrorCode code, const std::string& detail = "");

/// Coarse ParserErrorCode → StatusCode mapping used by ParserStatus (exposed
/// for tests / boundary code that needs the mapping without a message).
StatusCode ParserErrorToStatusCode(ParserErrorCode code);

}  // namespace cortrix::spc
