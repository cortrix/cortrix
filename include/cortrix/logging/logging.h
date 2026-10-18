#pragma once
#include <string>
#include "spdlog/spdlog.h"

namespace cortrix {

struct LogConfig;  // forward decl

/// Initialize logging system
void InitLogging(const LogConfig& config);

/// Flush every logger. The loggers stay in place until the process exits (or until
/// the next InitLogging, which drops them first), because threads that are still
/// stopping may write to them: a line logged after this call is written, not a crash.
/// A log file therefore stays open until then; on a normal exit spdlog's registry
/// closes it. Lines written after the last flush reach the file within a second
/// (the periodic flusher) or at that exit, WARN and above at once; console output
/// is flushed line by line.
void ShutdownLogging();

}  // namespace cortrix

// Convenience macros with module field
#define CORTRIX_LOG_TRACE(module, ...) SPDLOG_TRACE("[{}] {}", module, fmt::format(__VA_ARGS__))
#define CORTRIX_LOG_DEBUG(module, ...) SPDLOG_DEBUG("[{}] {}", module, fmt::format(__VA_ARGS__))
#define CORTRIX_LOG_INFO(module, ...)  SPDLOG_INFO("[{}] {}", module, fmt::format(__VA_ARGS__))
#define CORTRIX_LOG_WARN(module, ...)  SPDLOG_WARN("[{}] {}", module, fmt::format(__VA_ARGS__))
#define CORTRIX_LOG_ERROR(module, ...) SPDLOG_ERROR("[{}] {}", module, fmt::format(__VA_ARGS__))
