#pragma once
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"

namespace cortrix::deploy {

/// OpenMetrics `reason` controlled vocabulary (this enum is the maintained
/// SoT). Every `reason` metric label
/// must be one of these enum values, in the `<subsystem>.<action_outcome>`
/// pattern (lowercase + dot) — this prevents label cardinality explosion and
/// keeps cross-subsystem queries friendly.
///
/// 🚨 The `category` enum (auth/quota/transient/permanent/timeout) is
/// shared verbatim with the Agent-friendly error body (cortrix::agent_friendly::
/// ErrorCategory) — the same 5 values are enforced across the metric and error
/// channels so an Agent can pivot from a monitoring signal to an error
/// investigation. We therefore re-use ErrorCategory here rather than defining a
/// second enum.
///
/// Naming alignment: the dotted `reason` label maps 1:1 to an
/// UPPER_SNAKE `CX_ERR_*` error code, so an Agent can convert between the two
/// channels by a simple transform (ReasonToErrorCode / ErrorCodeToReason below).
///   llm.budget_exceeded     <-> CX_ERR_LLM_BUDGET_EXCEEDED
///   spc.queue_full          <-> CX_ERR_SPC_QUEUE_FULL
///   catalog.bf_not_ready    <-> CX_ERR_BF_NOT_READY
///   ns_routing.unauthorized <-> CX_ERR_NS_UNAUTHORIZED
enum class MetricReason {
    // llm.*
    kLlmApiKeyMissing,
    kLlmTimeout,
    kLlmBudgetExceeded,
    kLlmRateLimit,
    kLlmInvalidResponse,
    kLlmConnectionFailed,
    // spc.*
    kSpcQueueFull,
    kSpcWorkerCrashed,
    kSpcTaskTimeout,
    kSpcTaskCancelled,
    // disk.*
    kDiskThresholdWarn,
    kDiskThresholdCrit,
    kDiskWriteFailed,
    // vector_index.*
    kVectorIndexRebuildInProgress,
    kVectorIndexSearchTimeout,
    kVectorIndexShardUnavailable,
    // catalog.*
    kCatalogLookupFailed,
    kCatalogBfNotReady,
    // ns_routing.*
    kNsRoutingUnauthorized,
    kNsRoutingInvalidNs,
    kNsRoutingQuotaExceeded,
    // async.*
    kTaskAsyncTaskCancelled,
    kTaskAsyncSchedulerBusy,
};

/// Total entries in the controlled vocabulary. Compile-time anchor for
/// the regression test (the set must not silently shrink).
constexpr int kMetricReasonCount = 23;

/// The dotted `<subsystem>.<action_outcome>` string for `reason` (the value that
/// goes in the metric label). Total over the enum.
const char* ReasonString(MetricReason reason);

/// The subsystem prefix of `reason` (e.g. "llm", "disk", "catalog"). Total.
const char* ReasonSubsystem(MetricReason reason);

/// Every vocabulary entry, for iteration (tests / a CI lint, TD-REASON-
/// VOCABULARY-CI-LINT in Phase 2). Stable order = enum order.
const std::vector<MetricReason>& AllReasons();

/// True iff `s` is a member of the controlled vocabulary (guards a label before
/// it is emitted — an unknown reason is a design defect).
bool IsValidReason(const std::string& s);

/// Transform: dotted reason → UPPER_SNAKE `CX_ERR_*` code. Pure string
/// transform ("llm.budget_exceeded" → "CX_ERR_LLM_BUDGET_EXCEEDED"); does not
/// assert the code exists in any registry (the alignment rule is syntactic).
std::string ReasonToErrorCode(const std::string& reason);

/// Inverse transform: `CX_ERR_*` code → dotted reason. Strips the
/// CX_ERR_ prefix and lowercases ("CX_ERR_SPC_QUEUE_FULL" → "spc.queue_full"),
/// treating the first token after the prefix as the subsystem.
std::string ErrorCodeToReason(const std::string& error_code);

// --------------------------- label vs structured_data ---------------------------

/// Whether a field of a given semantic kind belongs on a metric label, in the
/// error structured_data, or both (the field-ownership table). Used to audit
/// that high-cardinality fields never become labels (OBS_SPEC §3.2).
enum class FieldChannel {
    kLabelOnly,         ///< low-cardinality enum: metric label only
    kStructuredOnly,    ///< high-cardinality / business data: structured_data only
    kBoth,              ///< category (5 enum) — enforced consistent across both (§9.2)
};

/// The semantic field kinds the §9.2 table classifies.
enum class FieldKind {
    kCategory,          ///< the 5-value category enum            → both (consistent)
    kReason,            ///< the controlled reason vocabulary     → label (→ error_code in errors)
    kPlanRegionInstance,///< plan / region / instance_id          → both
    kHighCardId,        ///< ns_id / user_id / doc_id / trace_id / session_id → structured_data only
    kNumericValue,      ///< budget / latency / size (a metric *value*)       → structured_data only
    kBusinessObject,    ///< business object / stack trace        → structured_data only
};

/// The §9.2 channel for `kind`. Total. The SoT for the field-ownership audit.
FieldChannel ChannelFor(FieldKind kind);

/// The 5 category enum values, as the lowercase strings shared across the metric
/// `category` label and the error body `category` field.
/// Re-exposes agent_friendly::ErrorCategory's serialization so a metric emitter
/// and an error emitter cannot drift.
const char* CategoryString(agent_friendly::ErrorCategory category);

}  // namespace cortrix::deploy
