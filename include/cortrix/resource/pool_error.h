#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::resource {

/// The 4 NS-pool error identities (PoolError). Each maps to a
/// stable `CX_ERR_NS_*` string + a GEN-Agent category + retryability via the
/// canonical registry below — the exact pattern CatalogErrorCode uses (
/// is the template, per L1_ALPHA_BRIEFING §2).
///
/// Per CODING_CONVENTIONS §3, Cortrix uses Result<T> + Status only (no
/// Result<T,E>): the spec's `Result<…, PoolError>` is read as Result<T> / Status,
/// and a domain error is carried as cortrix::agent_friendly::AgentFriendlyError,
/// identified by its CX_ERR_* code. So PoolErrorCode is the *enum of identities*
/// and MakePoolError() turns one (plus structured_data) into the boundary error.
///
/// Naming (§8.1 m4): the prefix is `CX_ERR_NS_*`, NOT `CX_ERR_F05_*` — admission
/// control failures are namespace-domain errors shared with the catalog and tenant layers, so an
/// Agent never filters Cortrix errors by F-module number. The 3 admission codes
/// reuse the same CX_ERR_NS_* strings the catalog already registers (catalog_error.h);
/// they are the same wire identities seen from the pool side.
///
/// Note POOL_INTERNAL is deliberately absent: the PoolError enum is the 4
/// values below; `CX_ERR_NS_POOL_INTERNAL` is an F12-side mapping fall-through
/// code, not an identity the pool itself returns.
enum class PoolErrorCode {
    kNsQuotaExceeded,           ///< NS count > max_namespaces_per_instance
    kNsResourceBudgetExceeded,  ///< would exceed memory_budget_bytes
    kNsLoadFailed,              ///< load failed (startup / admin retry)
    kNsNotFound,                ///< NS not in pool (never created / load failed)
};

/// Count of pool error codes (= 4). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink / be re-ordered).
constexpr int kPoolErrorCodeCount = 4;

/// Canonical, immutable attributes of one pool error code (mirrors
/// catalog::CatalogErrorInfo). retry_after_ms is null unless retryable.
struct PoolErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_NS_*" string
    agent_friendly::ErrorCategory category;   ///< quota / transient / ...
    bool retryable;
    std::optional<int> retry_after_ms;
};

/// Canonical attributes for `code`. Total over the enum (never throws / partial).
const PoolErrorInfo& GetPoolErrorInfo(PoolErrorCode code);

/// The "CX_ERR_NS_*" string for `code`.
const char* PoolErrorCodeString(PoolErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry ("required keys").
/// SoT for the Agent-friendly contract (GEN-Agent #5); lets call sites + tests
/// verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(PoolErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(PoolErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code` (§8.3 response bodies).
/// category / retryable / retry_after_ms come from the registry — call sites
/// supply only structured_data (the §8.1 required keys) and an optional message.
agent_friendly::AgentFriendlyError MakePoolError(
    PoolErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Coarse PoolErrorCode → StatusCode mapping (exposed for tests / boundary code).
/// quota → kPermissionDenied (closest coarse code; rich category=quota is kept
/// via the CX_ERR_ token); load-failed (transient) → kUnavailable; not-found →
/// kNotFound. Matches catalog_error.cpp's mapping for the shared CX_ERR_NS_* codes.
StatusCode PoolErrorToStatusCode(PoolErrorCode code);

/// Bridge a pool error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The message is prefixed with the CX_ERR_NS_* token
/// ("CX_ERR_NS_X: detail") so the exact identity is recoverable at the API/SDK
/// boundary, which re-inflates the full Agent-friendly body via MakePoolError().
Status PoolStatus(PoolErrorCode code, const std::string& detail = "");

}  // namespace cortrix::resource
