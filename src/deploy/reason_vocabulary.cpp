#include "cortrix/deploy/reason_vocabulary.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace cortrix::deploy {

namespace {

// One canonical dotted string per reason (F24 §8.2 / OBS_SPEC §3.bis). The switch
// is exhaustive so -Wswitch turns "added a reason without a row" into a build
// failure — the vocabulary cannot silently drift from the enum.
const char* ReasonStringImpl(MetricReason r) {
    switch (r) {
        case MetricReason::kLlmApiKeyMissing:            return "llm.api_key_missing";
        case MetricReason::kLlmTimeout:                  return "llm.timeout";
        case MetricReason::kLlmBudgetExceeded:           return "llm.budget_exceeded";
        case MetricReason::kLlmRateLimit:                return "llm.rate_limit";
        case MetricReason::kLlmInvalidResponse:          return "llm.invalid_response";
        case MetricReason::kLlmConnectionFailed:         return "llm.connection_failed";
        case MetricReason::kSpcQueueFull:                return "spc.queue_full";
        case MetricReason::kSpcWorkerCrashed:            return "spc.worker_crashed";
        case MetricReason::kSpcTaskTimeout:              return "spc.task_timeout";
        case MetricReason::kSpcTaskCancelled:            return "spc.task_cancelled";
        case MetricReason::kDiskThresholdWarn:           return "disk.threshold_warn";
        case MetricReason::kDiskThresholdCrit:           return "disk.threshold_crit";
        case MetricReason::kDiskWriteFailed:             return "disk.write_failed";
        case MetricReason::kVectorIndexRebuildInProgress:return "vector_index.rebuild_in_progress";
        case MetricReason::kVectorIndexSearchTimeout:    return "vector_index.search_timeout";
        case MetricReason::kVectorIndexShardUnavailable: return "vector_index.shard_unavailable";
        case MetricReason::kCatalogLookupFailed:         return "catalog.lookup_failed";
        case MetricReason::kCatalogBfNotReady:           return "catalog.bf_not_ready";
        case MetricReason::kNsRoutingUnauthorized:       return "ns_routing.unauthorized";
        case MetricReason::kNsRoutingInvalidNs:          return "ns_routing.invalid_ns";
        case MetricReason::kNsRoutingQuotaExceeded:      return "ns_routing.quota_exceeded";
        case MetricReason::kF42AsyncTaskCancelled:       return "f42_async.task_cancelled";
        case MetricReason::kF42AsyncSchedulerBusy:       return "f42_async.scheduler_busy";
    }
    return "";  // unreachable for a valid enum
}

}  // namespace

const char* ReasonString(MetricReason reason) {
    return ReasonStringImpl(reason);
}

const char* ReasonSubsystem(MetricReason reason) {
    switch (reason) {
        case MetricReason::kLlmApiKeyMissing:
        case MetricReason::kLlmTimeout:
        case MetricReason::kLlmBudgetExceeded:
        case MetricReason::kLlmRateLimit:
        case MetricReason::kLlmInvalidResponse:
        case MetricReason::kLlmConnectionFailed:         return "llm";
        case MetricReason::kSpcQueueFull:
        case MetricReason::kSpcWorkerCrashed:
        case MetricReason::kSpcTaskTimeout:
        case MetricReason::kSpcTaskCancelled:            return "spc";
        case MetricReason::kDiskThresholdWarn:
        case MetricReason::kDiskThresholdCrit:
        case MetricReason::kDiskWriteFailed:             return "disk";
        case MetricReason::kVectorIndexRebuildInProgress:
        case MetricReason::kVectorIndexSearchTimeout:
        case MetricReason::kVectorIndexShardUnavailable: return "vector_index";
        case MetricReason::kCatalogLookupFailed:
        case MetricReason::kCatalogBfNotReady:           return "catalog";
        case MetricReason::kNsRoutingUnauthorized:
        case MetricReason::kNsRoutingInvalidNs:
        case MetricReason::kNsRoutingQuotaExceeded:      return "ns_routing";
        case MetricReason::kF42AsyncTaskCancelled:
        case MetricReason::kF42AsyncSchedulerBusy:       return "f42_async";
    }
    return "";  // unreachable
}

const std::vector<MetricReason>& AllReasons() {
    static const std::vector<MetricReason> kAll = {
        MetricReason::kLlmApiKeyMissing,            MetricReason::kLlmTimeout,
        MetricReason::kLlmBudgetExceeded,           MetricReason::kLlmRateLimit,
        MetricReason::kLlmInvalidResponse,          MetricReason::kLlmConnectionFailed,
        MetricReason::kSpcQueueFull,                MetricReason::kSpcWorkerCrashed,
        MetricReason::kSpcTaskTimeout,              MetricReason::kSpcTaskCancelled,
        MetricReason::kDiskThresholdWarn,           MetricReason::kDiskThresholdCrit,
        MetricReason::kDiskWriteFailed,             MetricReason::kVectorIndexRebuildInProgress,
        MetricReason::kVectorIndexSearchTimeout,    MetricReason::kVectorIndexShardUnavailable,
        MetricReason::kCatalogLookupFailed,         MetricReason::kCatalogBfNotReady,
        MetricReason::kNsRoutingUnauthorized,       MetricReason::kNsRoutingInvalidNs,
        MetricReason::kNsRoutingQuotaExceeded,      MetricReason::kF42AsyncTaskCancelled,
        MetricReason::kF42AsyncSchedulerBusy,
    };
    return kAll;
}

bool IsValidReason(const std::string& s) {
    static const std::unordered_set<std::string> kSet = [] {
        std::unordered_set<std::string> set;
        for (MetricReason r : AllReasons()) set.insert(ReasonString(r));
        return set;
    }();
    return kSet.count(s) > 0;
}

std::string ReasonToErrorCode(const std::string& reason) {
    std::string out = "CX_ERR_";
    out.reserve(reason.size() + 7);
    for (char c : reason) {
        if (c == '.') out += '_';
        else out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string ErrorCodeToReason(const std::string& error_code) {
    // Strip a leading "CX_ERR_"; the first remaining token (up to the next '_')
    // is the subsystem, the rest is the action_outcome. We lowercase everything
    // and replace the *first* '_' with '.', leaving the rest as underscores.
    constexpr const char* kPrefix = "CX_ERR_";
    std::string body = error_code;
    if (body.rfind(kPrefix, 0) == 0) body = body.substr(7);

    std::string lower;
    lower.reserve(body.size());
    for (char c : body) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto first_us = lower.find('_');
    if (first_us == std::string::npos) return lower;  // no action part
    lower[first_us] = '.';
    return lower;
}

FieldChannel ChannelFor(FieldKind kind) {
    switch (kind) {
        case FieldKind::kCategory:            return FieldChannel::kBoth;      // §9.2 (consistent)
        case FieldKind::kReason:              return FieldChannel::kLabelOnly; // → error_code in errors
        case FieldKind::kPlanRegionInstance:  return FieldChannel::kBoth;
        case FieldKind::kHighCardId:          return FieldChannel::kStructuredOnly;
        case FieldKind::kNumericValue:        return FieldChannel::kStructuredOnly;
        case FieldKind::kBusinessObject:      return FieldChannel::kStructuredOnly;
    }
    return FieldChannel::kStructuredOnly;  // unreachable
}

const char* CategoryString(agent_friendly::ErrorCategory category) {
    // Delegate to the single serialization in agent_friendly so the metric
    // `category` label and the error `category` field never diverge (F24-7).
    return agent_friendly::ToString(category);
}

}  // namespace cortrix::deploy
