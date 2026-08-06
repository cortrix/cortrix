#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::catalog {

/// The 24 catalog error identities. Each maps to a stable `CX_ERR_*`
/// string + a GEN-Agent category + retryability via the canonical registry below.
///
/// Per the coding conventions, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// So CatalogErrorCode is the *enum of identities*, and MakeCatalogError() turns
/// one (plus optional structured_data) into that boundary error — we do NOT
/// introduce a parallel CatalogError struct (the `Result<T,CatalogError>`
/// spelling is the pre-convention draft; this is its convention-compliant form).
///
/// V1.0 versioning promise: this set is not removed /
/// renamed / re-categorized; new codes may be appended (api_version stays "v1").
/// Three codes are Phase-1-reserved (present, but never returned until Phase 2):
/// kUnitArchived / kUnitOwnerRemote / kTenantQuotaExceeded.
enum class CatalogErrorCode {
    // --- NS (namespace) ---
    kNsNotFound,
    kNsUnauthorized,
    kNsIsolated,
    kNsVisibilityDenied,
    kNsAlreadyExists,
    // --- Unit ---
    kUnitNotFound,
    kUnitNotSealed,
    kUnitArchived,        // Phase 2 reserved
    kUnitOwnerRemote,     // Phase 2 reserved
    // --- Tenant ---
    kTenantNotFound,
    kTenantQuotaExceeded, // Phase 2 reserved
    // --- Schema / migration ---
    kSchemaVersionMismatch,
    kSchemaMigrationFailed,
    // --- Config / locking / transaction ---
    kInvalidConfigJson,
    kCatalogLocked,
    kBloomFilterNotReady,
    kTransactionRetry,
    // --- Integrity ---
    kContentHashMismatch,
    kRefCountNegative,
    // --- Generic ---
    kInternalError,
    kNotImplemented,
    // --- namespace-pool admission ---
    kNsQuotaExceeded,
    kNsResourceBudgetExceeded,
    kNsPoolInternal,
};

/// Total number of catalog error codes (= 24). Compile-time anchor for
/// the API-compatibility regression test (the set must not shrink).
constexpr int kCatalogErrorCodeCount = 24;

/// Canonical, immutable attributes of one error code.
struct CatalogErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 24 rows.
const CatalogErrorInfo& GetCatalogErrorInfo(CatalogErrorCode code);

/// The "CX_ERR_*" string for `code` (convenience over GetCatalogErrorInfo).
const char* CatalogErrorCodeString(CatalogErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry ("required keys"
/// column). Empty for CX_ERR_INTERNAL_ERROR ("case-by-case"). This
/// is the SoT for the Agent-friendly contract (GEN-Agent #5) and lets call sites
/// + tests verify the body is complete (S5.1 / topic 5).
const std::vector<std::string>& RequiredStructuredDataKeys(CatalogErrorCode code);

/// True iff `structured_data` contains every required key for `code`. Used to
/// assert Agent-friendly completeness before returning an error body.
bool HasRequiredStructuredData(CatalogErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the required keys are the caller's responsibility at each call site) and
/// an optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeCatalogError(
    CatalogErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge a catalog error to a plain Status for the Result<T>/Status interface
/// surface (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the
/// message is prefixed with the CX_ERR_* token ("CX_ERR_X: detail") so the exact
/// catalog identity is recoverable at the API/SDK boundary, which re-inflates the
/// full Agent-friendly body (category / retryable / structured_data) via
/// MakeCatalogError(). We deliberately do NOT widen cortrix::Status itself (that
/// would be a cross-cutting change to the shared primitive — out of catalog scope).
Status CatalogStatus(CatalogErrorCode code, const std::string& detail = "");

/// Coarse CatalogErrorCode → StatusCode mapping used by CatalogStatus (exposed
/// for tests / boundary code that needs the mapping without a message).
StatusCode CatalogErrorToStatusCode(CatalogErrorCode code);

}  // namespace cortrix::catalog
