#include <gtest/gtest.h>
#include "cortrix/logging/logging.h"
#include "cortrix/config/config.h"

#include <fstream>
#include <sstream>
#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <vector>

#include <unistd.h>

namespace cortrix {
namespace {

// Helper: capture stdout to string
class StdoutCapture {
public:
    StdoutCapture() {
        old_ = std::cout.rdbuf(buffer_.rdbuf());
    }
    ~StdoutCapture() {
        std::cout.rdbuf(old_);
    }
    std::string Get() const {
        return buffer_.str();
    }
private:
    std::stringstream buffer_;
    std::streambuf* old_;
};

class LoggingTest : public ::testing::Test {
protected:
    void TearDown() override {
        ShutdownLogging();
        // Re-initialize a minimal default logger so that subsequent test
        // suites using SPDLOG macros don't crash on a null logger pointer.
        LogConfig cfg;
        cfg.level = "info";
        cfg.output = "stderr";
        cfg.format = "text";
        InitLogging(cfg);
    }
};

TEST_F(LoggingTest, InitWithJsonFormat) {
    LogConfig config;
    config.level = "info";
    config.format = "json";
    config.output = "stdout";

    EXPECT_NO_THROW(InitLogging(config));

    // Logging should be initialized without errors
    CORTRIX_LOG_INFO("test", "Test message");

    ShutdownLogging();
}

TEST_F(LoggingTest, InitWithTextFormat) {
    LogConfig config;
    config.level = "info";
    config.format = "text";
    config.output = "stdout";

    EXPECT_NO_THROW(InitLogging(config));
    CORTRIX_LOG_INFO("test", "Test message");
}

TEST_F(LoggingTest, LogLevelFiltering) {
    const std::string log_file = "/tmp/cortrix_test_log_filter.txt";
    std::remove(log_file.c_str());

    LogConfig config;
    config.level = "warn";  // Set to warn level
    config.format = "text";
    config.output = log_file;

    InitLogging(config);
    CORTRIX_LOG_DEBUG("test", "debug-marker-filtered");
    CORTRIX_LOG_INFO("test", "info-marker-filtered");
    CORTRIX_LOG_WARN("test", "warn-marker-kept");
    CORTRIX_LOG_ERROR("test", "error-marker-kept");
    ShutdownLogging();

    std::ifstream file(log_file);
    ASSERT_TRUE(file.is_open());
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();
    EXPECT_EQ(content.find("debug-marker-filtered"), std::string::npos);
    EXPECT_EQ(content.find("info-marker-filtered"), std::string::npos);
    EXPECT_NE(content.find("warn-marker-kept"), std::string::npos);
    EXPECT_NE(content.find("error-marker-kept"), std::string::npos);

    std::remove(log_file.c_str());
}

TEST_F(LoggingTest, AllLogLevels) {
    LogConfig config;
    config.level = "trace";  // Enable all levels
    config.format = "json";
    config.output = "stdout";

    InitLogging(config);

    EXPECT_NO_THROW({
        CORTRIX_LOG_TRACE("test", "Trace message");
        CORTRIX_LOG_DEBUG("test", "Debug message");
        CORTRIX_LOG_INFO("test", "Info message");
        CORTRIX_LOG_WARN("test", "Warn message");
        CORTRIX_LOG_ERROR("test", "Error message");
    });
}

TEST_F(LoggingTest, LogWithFormatting) {
    LogConfig config;
    config.level = "info";
    config.format = "json";
    config.output = "stdout";

    InitLogging(config);

    // Test format string support
    EXPECT_NO_THROW({
        CORTRIX_LOG_INFO("test", "Message with int: {}", 42);
        CORTRIX_LOG_INFO("test", "Message with string: {}", "hello");
        CORTRIX_LOG_INFO("test", "Message with multiple: {} {}", 1, "test");
    });
}

TEST_F(LoggingTest, FileOutput) {
    const std::string log_file = "/tmp/cortrix_test_log.txt";

    // Clean up any existing log file
    std::remove(log_file.c_str());

    LogConfig config;
    config.level = "info";
    config.format = "text";
    config.output = log_file;

    InitLogging(config);
    CORTRIX_LOG_INFO("test", "Test message to file");
    ShutdownLogging();

    // Verify file was created and contains content
    std::ifstream file(log_file);
    ASSERT_TRUE(file.is_open());

    std::string content;
    std::getline(file, content);
    EXPECT_FALSE(content.empty());

    file.close();
    std::remove(log_file.c_str());
}

TEST_F(LoggingTest, ModuleTagging) {
    const std::string log_file = "/tmp/cortrix_test_log_module.txt";
    std::remove(log_file.c_str());

    LogConfig config;
    config.level = "info";
    config.format = "text";
    config.output = log_file;

    InitLogging(config);
    CORTRIX_LOG_INFO("server", "Server message");
    CORTRIX_LOG_INFO("auth", "Auth message");
    CORTRIX_LOG_INFO("storage", "Storage message");
    ShutdownLogging();

    std::ifstream file(log_file);
    ASSERT_TRUE(file.is_open());
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();
    // Each line carries its "[module]" tag next to its message (the CORTRIX_LOG
    // macros render "[{module}] {message}").
    EXPECT_NE(content.find("[server]"), std::string::npos);
    EXPECT_NE(content.find("[auth]"), std::string::npos);
    EXPECT_NE(content.find("[storage]"), std::string::npos);
    EXPECT_NE(content.find("Server message"), std::string::npos);
    EXPECT_NE(content.find("Auth message"), std::string::npos);
    EXPECT_NE(content.find("Storage message"), std::string::npos);

    std::remove(log_file.c_str());
}

TEST_F(LoggingTest, ShutdownIsIdempotent) {
    LogConfig config;
    config.level = "info";
    config.format = "json";
    config.output = "stdout";

    InitLogging(config);

    // Multiple shutdowns should not crash
    EXPECT_NO_THROW({
        ShutdownLogging();
        ShutdownLogging();
        ShutdownLogging();
    });
}

TEST_F(LoggingTest, ReinitializeAfterShutdown) {
    LogConfig config;
    config.level = "info";
    config.format = "json";
    config.output = "stdout";

    InitLogging(config);
    CORTRIX_LOG_INFO("test", "First init");
    ShutdownLogging();

    // Re-initialize should work
    InitLogging(config);
    CORTRIX_LOG_INFO("test", "Second init");
    ShutdownLogging();
}

TEST_F(LoggingTest, InvalidLogLevelDefaultsToInfo) {
    const std::string log_file = "/tmp/cortrix_test_log_badlevel.txt";
    std::remove(log_file.c_str());

    LogConfig config;
    config.level = "invalid_level";  // ParseLevel falls back to info
    config.format = "text";
    config.output = log_file;

    InitLogging(config);
    // At the info default: debug is filtered, info passes.
    CORTRIX_LOG_DEBUG("test", "badlevel-debug-filtered");
    CORTRIX_LOG_INFO("test", "badlevel-info-kept");
    ShutdownLogging();

    std::ifstream file(log_file);
    ASSERT_TRUE(file.is_open());
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();
    EXPECT_EQ(content.find("badlevel-debug-filtered"), std::string::npos);
    EXPECT_NE(content.find("badlevel-info-kept"), std::string::npos);

    std::remove(log_file.c_str());
}

TEST_F(LoggingTest, DefaultLogConfig) {
    LogConfig config;  // Use all defaults

    EXPECT_EQ(config.level, "info");
    EXPECT_EQ(config.format, "json");
    EXPECT_EQ(config.output, "stdout");

    EXPECT_NO_THROW(InitLogging(config));
}

TEST_F(LoggingTest, StderrOutput) {
    LogConfig config;
    config.level = "info";
    config.format = "text";
    config.output = "stderr";

    EXPECT_NO_THROW(InitLogging(config));
    CORTRIX_LOG_INFO("test", "Stderr output test");
}

TEST_F(LoggingTest, TextFormatWithAllLevels) {
    LogConfig config;
    config.level = "trace";
    config.format = "text";
    config.output = "stdout";

    InitLogging(config);

    // Text format should work with all log levels without crashing
    EXPECT_NO_THROW({
        CORTRIX_LOG_TRACE("test", "Trace in text format");
        CORTRIX_LOG_DEBUG("test", "Debug in text format");
        CORTRIX_LOG_INFO("test", "Info in text format");
        CORTRIX_LOG_WARN("test", "Warn in text format");
        CORTRIX_LOG_ERROR("test", "Error in text format");
    });
}

TEST_F(LoggingTest, EmptyModuleTag) {
    LogConfig config;
    config.level = "info";
    config.format = "json";
    config.output = "stdout";

    InitLogging(config);

    // Empty module tag should not crash
    EXPECT_NO_THROW({
        CORTRIX_LOG_INFO("", "Message with empty module");
    });
}

TEST_F(LoggingTest, OffLevelSuppressesAllMessages) {
    const std::string log_file = "/tmp/cortrix_test_off_level.txt";
    std::remove(log_file.c_str());

    LogConfig config;
    config.level = "off";
    config.format = "text";
    config.output = log_file;

    InitLogging(config);

    // These should all be suppressed
    CORTRIX_LOG_TRACE("test", "trace msg");
    CORTRIX_LOG_DEBUG("test", "debug msg");
    CORTRIX_LOG_INFO("test", "info msg");
    CORTRIX_LOG_WARN("test", "warn msg");
    CORTRIX_LOG_ERROR("test", "error msg");

    ShutdownLogging();

    // File should be empty or not contain any messages
    std::ifstream file(log_file);
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    EXPECT_TRUE(content.empty()) << "off level should suppress all messages";
    file.close();
    std::remove(log_file.c_str());
}

// ---------------------------------------------------------------------------
// Logging after ShutdownLogging() (#196). ShutdownLogging() used to call
// spdlog::shutdown(), which drops the default logger; the logging macros and
// spdlog's free functions dereference it unchecked, so a line written afterwards
// (a worker still finishing its task) killed the process. ShutdownLogging() now
// flushes and keeps the loggers. Each statement runs in a re-executed child
// (threadsafe style, so no flusher thread is inherited from earlier tests in this
// process), initialises logging itself and reports through its exit code.
// ---------------------------------------------------------------------------

std::string AfterShutdownLogPath(pid_t parent, const char* tag) {
    return ::testing::TempDir() + "cortrix_log_after_shutdown_" + tag + "_" +
           std::to_string(parent) + ".txt";
}

[[noreturn]] void LogLinesAfterShutdown() {
    alarm(60);
    const std::string path = AfterShutdownLogPath(getppid(), "lines");
    LogConfig cfg;
    cfg.level = "info";
    cfg.format = "text";
    cfg.output = path;
    InitLogging(cfg);
    ShutdownLogging();
    CORTRIX_LOG_WARN("test", "marker-warn-after-shutdown");
    CORTRIX_LOG_ERROR("test", "marker-error-after-shutdown");
    spdlog::warn("marker-direct-after-shutdown");
    // Last, so that only the next ShutdownLogging() writes it out: WARN and ERROR
    // flush on their own and would carry an earlier INFO line with them.
    CORTRIX_LOG_INFO("test", "marker-info-after-shutdown");
    ShutdownLogging();
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    in.close();
    std::remove(path.c_str());
    const std::string s = text.str();
    const bool all = s.find("marker-warn-after-shutdown") != std::string::npos &&
                     s.find("marker-error-after-shutdown") != std::string::npos &&
                     s.find("marker-direct-after-shutdown") != std::string::npos &&
                     s.find("marker-info-after-shutdown") != std::string::npos;
    std::_Exit(all ? 0 : 3);
}

[[noreturn]] void ThreadsLogAcrossShutdown() {
    alarm(120);
    const std::string path = AfterShutdownLogPath(getppid(), "threads");
    LogConfig cfg;
    cfg.level = "info";
    cfg.format = "text";
    cfg.output = path;
    InitLogging(cfg);
    std::atomic<bool> start{false};
    std::atomic<int> done{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&start, &done, t] {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 20000; ++i) {
                if (i % 2 == 0) {
                    spdlog::warn("writer {} line {}", t, i);
                } else {
                    CORTRIX_LOG_INFO("test", "writer {} line {}", t, i);
                }
            }
            done.fetch_add(1, std::memory_order_acq_rel);
        });
    }
    ShutdownLogging();
    start.store(true, std::memory_order_release);
    // Keep shutting down for as long as anyone writes, so the calls cover the whole
    // write period whatever the scheduling.
    while (done.load(std::memory_order_acquire) < 4) ShutdownLogging();
    for (auto& w : writers) w.join();
    std::remove(path.c_str());
    std::_Exit(0);
}

TEST(LoggingAfterShutdownDeathTest, LinesAfterShutdownReachTheFile) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(LogLinesAfterShutdown(), ::testing::ExitedWithCode(0), "");
    std::remove(AfterShutdownLogPath(getpid(), "lines").c_str());
}

TEST(LoggingAfterShutdownDeathTest, ThreadsLoggingAcrossShutdownSurvive) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(ThreadsLogAcrossShutdown(), ::testing::ExitedWithCode(0), "");
    std::remove(AfterShutdownLogPath(getpid(), "threads").c_str());
}

}  // namespace
}  // namespace cortrix
