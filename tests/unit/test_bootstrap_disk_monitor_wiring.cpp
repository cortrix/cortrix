// Regression coverage for issue #148: the disk monitor's thresholds and check
// interval must reach the monitor the SERVER constructs. The helper that read
// them had a green unit test while the server passed it nothing, so a test that
// hands the helper a value cannot catch this. This test boots the real
// RunServer() entry (stub model mode) with the three variables set and reads the
// effective values back from the startup log, the one place the production path
// reports them. Removing the reading of a variable, or the log line, turns it
// red.
#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <httplib.h>
#include <unistd.h>

#include "cortrix/config/config.h"
#include "cortrix/logging/logging.h"
#include "cortrix/server/bootstrap.h"

namespace cortrix {
namespace {

std::string Slurp(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The three variables are named by their public spelling on purpose: this test
// pins what an operator types, not a constant the implementation could rename.
constexpr const char* kDiskEnv[] = {"CORTRIX_DISK_WARN_THRESHOLD",
                                    "CORTRIX_DISK_CRIT_THRESHOLD",
                                    "CORTRIX_DISK_CHECK_INTERVAL_SEC"};

class BootstrapDiskMonitorWiringTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = std::filesystem::temp_directory_path() /
                ("cortrix_bootstrap_disk_" + std::to_string(getpid()));
        data_dir_ = base_ / "data";
        log_file_ = base_ / "server.log";
        config_path_ = base_ / "config.yaml";
        // Ports distinct from the other in-process boot in this binary (18000 +
        // pid % 10000) and from the metrics default 9091, so two sequential boots
        // never contend for a listener.
        port_ = 19000 + (getpid() % 10000);
        metrics_port_ = 29000 + (getpid() % 10000);
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
        std::filesystem::create_directories(data_dir_);

        std::ofstream cfg(config_path_);
        cfg << "server:\n"
            << "  host: \"127.0.0.1\"\n"
            << "  port: " << port_ << "\n"
            << "namespace:\n"
            << "  data_dir: " << data_dir_.string() << "\n"
            << "log:\n"
            << "  level: \"info\"\n"
            << "  format: \"text\"\n"
            << "  output: " << log_file_.string() << "\n";
        cfg.close();
        // Hermetic: the config loader lets these override the file above, which
        // would silence or relocate the line this test reads.
        for (const char* v : {"CORTRIX_LOG_LEVEL", "CORTRIX_LOG_FORMAT", "CORTRIX_LOG_OUTPUT",
                              "CORTRIX_DATA_DIR"}) {
            unsetenv(v);
        }
        setenv("CORTRIX_METRICS_PORT", std::to_string(metrics_port_).c_str(), 1);
    }
    void TearDown() override {
        for (const char* v : kDiskEnv) unsetenv(v);
        unsetenv("CORTRIX_METRICS_PORT");
        // RunServer leaves its logger writing to this fixture's log file (its exit
        // path only flushes). Restore a default logger before the file is removed,
        // so later tests in a monolithic run log somewhere real.
        LogConfig lc;
        InitLogging(lc);
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    std::filesystem::path base_, data_dir_, log_file_, config_path_;
    int port_ = 0;
    int metrics_port_ = 0;
};

TEST_F(BootstrapDiskMonitorWiringTest, StartedServerHonoursTheDiskVariables) {
    setenv(kDiskEnv[0], "0.70", 1);
    setenv(kDiskEnv[1], "0.85", 1);
    setenv(kDiskEnv[2], "60", 1);

    // Boot the real server entry in-process.
    std::string program = "cortrix-server";
    std::string flag = "--config";
    std::string config_arg = config_path_.string();
    char* argv[] = {program.data(), flag.data(), config_arg.data()};
    int rc = -1;
    std::thread server([&] { rc = server::RunServer(3, argv); });

    // Wait until the HTTP server answers (any response = listening; the monitor is
    // constructed, and its line logged, long before the listener opens).
    bool up = false;
    for (int i = 0; i < 300 && !up; ++i) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(0, 200000);
        if (cli.Get("/api/v1/health")) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(up) << "server did not start listening on port " << port_;

    ::raise(SIGTERM);
    server.join();
    EXPECT_EQ(rc, 0) << "graceful shutdown did not return 0";

    const std::string log = Slurp(log_file_);
    EXPECT_NE(log.find("Disk monitor: warn=0.70 crit=0.85 interval=60s"), std::string::npos)
        << "the server's own disk monitor did not report the configured values. "
           "Startup log:\n"
        << log.substr(0, 4000);
}

}  // namespace
}  // namespace cortrix
