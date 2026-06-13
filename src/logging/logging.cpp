#include "cortrix/logging/logging.h"
#include "cortrix/config/config.h"

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/pattern_formatter.h"

#include <nlohmann/json.hpp>

namespace cortrix {

namespace {

spdlog::level::level_enum ParseLevel(const std::string& level) {
    if (level == "trace")    return spdlog::level::trace;
    if (level == "debug")    return spdlog::level::debug;
    if (level == "info")     return spdlog::level::info;
    if (level == "warn")     return spdlog::level::warn;
    if (level == "error")    return spdlog::level::err;
    if (level == "critical") return spdlog::level::critical;
    if (level == "off")      return spdlog::level::off;
    return spdlog::level::info;
}

class JsonFormatter : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg&, const std::tm&,
                spdlog::memory_buf_t&) override {}
    std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<JsonFormatter>();
    }
};

}  // namespace

void InitLogging(const LogConfig& config) {
    spdlog::drop_all();

    spdlog::sink_ptr sink;
    if (config.output == "stdout") {
        sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    } else if (config.output == "stderr") {
        sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    } else {
        sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.output, true);
    }

    auto logger = std::make_shared<spdlog::logger>("cortrix", sink);
    logger->set_level(ParseLevel(config.level));

    if (config.format == "json") {
        logger->set_pattern(R"({"timestamp":"%Y-%m-%dT%H:%M:%S.%eZ","level":"%l","message":"%v","thread":%t})");
    } else {
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [tid:%t] %v");
    }

    spdlog::set_default_logger(logger);
    spdlog::set_level(ParseLevel(config.level));
}

void ShutdownLogging() {
    spdlog::shutdown();
}

}  // namespace cortrix
