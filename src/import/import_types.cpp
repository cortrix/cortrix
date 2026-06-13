#include "cortrix/import/import_types.h"

namespace cortrix::import {

const char* ToString(TextStrategy strategy) {
    switch (strategy) {
        case TextStrategy::kPerRow: return "per_row";
        case TextStrategy::kMerge:  return "merge";
    }
    return "per_row";  // unreachable for a valid enum
}

std::optional<TextStrategy> ParseTextStrategy(const std::string& s) {
    if (s == "per_row") return TextStrategy::kPerRow;
    if (s == "merge") return TextStrategy::kMerge;
    // "template" and everything else is rejected in V1.0 (D1 V3 resolution 13).
    return std::nullopt;
}

const char* ToString(ImportTaskStatus status) {
    switch (status) {
        case ImportTaskStatus::kQueued:     return "queued";
        case ImportTaskStatus::kRunning:    return "running";
        case ImportTaskStatus::kCompleted:  return "completed";
        case ImportTaskStatus::kFailed:     return "failed";
        case ImportTaskStatus::kCancelling: return "cancelling";
        case ImportTaskStatus::kCancelled:  return "cancelled";
    }
    return "queued";  // unreachable for a valid enum
}

}  // namespace cortrix::import
