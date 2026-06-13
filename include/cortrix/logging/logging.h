#pragma once
#include <string>
#include "spdlog/spdlog.h"

namespace cortrix {

struct LogConfig;  // forward decl

/// Initialize logging system
void InitLogging(const LogConfig& config);

/// Shutdown logging system (flush + drop)
void ShutdownLogging();

}  // namespace cortrix

// Convenience macros with module field
#define CORTRIX_LOG_TRACE(module, ...) SPDLOG_TRACE("[{}] {}", module, fmt::format(__VA_ARGS__))
#define CORTRIX_LOG_DEBUG(module, ...) SPDLOG_DEBUG("[{}] {}", module, fmt::format(__VA_ARGS__))
#define CORTRIX_LOG_INFO(module, ...)  SPDLOG_INFO("[{}] {}", module, fmt::format(__VA_ARGS__))
#define CORTRIX_LOG_WARN(module, ...)  SPDLOG_WARN("[{}] {}", module, fmt::format(__VA_ARGS__))
#define CORTRIX_LOG_ERROR(module, ...) SPDLOG_ERROR("[{}] {}", module, fmt::format(__VA_ARGS__))
