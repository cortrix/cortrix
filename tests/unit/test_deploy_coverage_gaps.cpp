// Coverage-gap unit tests for src/deploy: f24_error.cpp (66%) and
// disk_monitor.cpp (66%).
//
// The existing test_f24_disk.cpp covers the DiskFull registry row + DiskFull
// structured-data keys + the FSM via CheckWithSample. This file fills the
// remaining branches:
//
//   f24_error.cpp:
//     - RequiredStructuredDataKeys for the 3 non-DiskFull codes (shutdown/health/bf)
//     - HasRequiredStructuredData: non-object input + per-code completeness
//     - F24ErrorHttpStatus for every code (table-driven)
//     - F24ErrorToStatusCode for every code (all 4 -> kUnavailable)
//     - F24Status with empty detail (the "no detail" message arm)
//
//   disk_monitor.cpp:
//     - ToString(DiskStage) for all three stages
//     - CheckOnce() real-statvfs success path against /tmp
//     - CheckOnce() fail-safe path (bad data_dir -> last sample returned unchanged)
//     - Usage() ratio reconstruction from the injected atomics
//     - Apply via CheckWithSample feeds the gauge + reject flag (NORMAL clears it)
//
// All deterministic: the only real-FS call is statvfs("/tmp"), which is a
// read-only stat (no thread / no watch). Suite names module-prefixed + unique.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "cortrix/deploy/deploy_metrics.h"
#include "cortrix/deploy/disk_monitor.h"
#include "cortrix/deploy/f24_error.h"

namespace cortrix::deploy {
namespace {

using agent_friendly::ErrorCategory;

constexpr F24ErrorCode kAllCodes[] = {
    F24ErrorCode::kDiskFull,
    F24ErrorCode::kShutdownInProgress,
    F24ErrorCode::kHealthCheckFailed,
    F24ErrorCode::kBfNotReady,
};

// --------------------- f24_error.cpp: structured-data keys ---------------------

TEST(F24ErrorGapTest, RequiredKeysForNonDiskFullCodes) {
    // ShutdownInProgress -> ["shutdown_status"]
    const auto& sd = RequiredStructuredDataKeys(F24ErrorCode::kShutdownInProgress);
    ASSERT_EQ(sd.size(), 1u);
    EXPECT_EQ(sd[0], "shutdown_status");

    // HealthCheckFailed -> ["component", "error_id"]
    const auto& he = RequiredStructuredDataKeys(F24ErrorCode::kHealthCheckFailed);
    ASSERT_EQ(he.size(), 2u);
    EXPECT_EQ(he[0], "component");
    EXPECT_EQ(he[1], "error_id");

    // BfNotReady -> ["rebuild_progress"]
    const auto& bf = RequiredStructuredDataKeys(F24ErrorCode::kBfNotReady);
    ASSERT_EQ(bf.size(), 1u);
    EXPECT_EQ(bf[0], "rebuild_progress");
}

TEST(F24ErrorGapTest, HasRequiredStructuredDataNonObjectIsFalseWhenKeysRequired) {
    // A non-object payload cannot satisfy a code that requires keys.
    nlohmann::json not_object = nlohmann::json::array({1, 2, 3});
    EXPECT_FALSE(HasRequiredStructuredData(F24ErrorCode::kBfNotReady, not_object));
    // Per-code completeness: present -> true, missing -> false.
    nlohmann::json ok = {{"rebuild_progress", 0.5}};
    EXPECT_TRUE(HasRequiredStructuredData(F24ErrorCode::kBfNotReady, ok));
    nlohmann::json wrong = {{"unrelated", 1}};
    EXPECT_FALSE(HasRequiredStructuredData(F24ErrorCode::kBfNotReady, wrong));

    // Health requires two keys: one present is still incomplete.
    nlohmann::json partial = {{"component", "bm25"}};
    EXPECT_FALSE(HasRequiredStructuredData(F24ErrorCode::kHealthCheckFailed, partial));
    nlohmann::json full = {{"component", "bm25"}, {"error_id", "e1"}};
    EXPECT_TRUE(HasRequiredStructuredData(F24ErrorCode::kHealthCheckFailed, full));
}

// --------------------- f24_error.cpp: code -> http / status ---------------------

TEST(F24ErrorGapTest, HttpStatusTableDrivenOverAllCodes) {
    EXPECT_EQ(F24ErrorHttpStatus(F24ErrorCode::kDiskFull), 507);
    EXPECT_EQ(F24ErrorHttpStatus(F24ErrorCode::kShutdownInProgress), 503);
    EXPECT_EQ(F24ErrorHttpStatus(F24ErrorCode::kHealthCheckFailed), 503);
    EXPECT_EQ(F24ErrorHttpStatus(F24ErrorCode::kBfNotReady), 503);
}

TEST(F24ErrorGapTest, ToStatusCodeAllCodesMapToUnavailable) {
    for (F24ErrorCode c : kAllCodes) {
        EXPECT_EQ(F24ErrorToStatusCode(c), StatusCode::kUnavailable);
    }
}

TEST(F24ErrorGapTest, F24StatusEmptyDetailUsesCodeAsMessage) {
    // Empty detail -> message is exactly the CX_ERR_* token (no ": detail" suffix).
    Status s = F24Status(F24ErrorCode::kBfNotReady);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_EQ(s.message(), std::string("CX_ERR_BF_NOT_READY"));
}

TEST(F24ErrorGapTest, MakeErrorEmptyMessageFallsBackToCode) {
    // message="" -> err.message defaults to the code token.
    auto err = MakeF24Error(F24ErrorCode::kHealthCheckFailed);
    EXPECT_EQ(err.code, "CX_ERR_HEALTH_CHECK_FAILED");
    EXPECT_EQ(err.message, "CX_ERR_HEALTH_CHECK_FAILED");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
}

// --------------------- disk_monitor.cpp: ToString + CheckOnce ---------------------

TEST(DiskMonitorGapTest, StageToStringForAllStages) {
    EXPECT_STREQ(ToString(DiskStage::kNormal), "normal");
    EXPECT_STREQ(ToString(DiskStage::kWarn), "warn");
    EXPECT_STREQ(ToString(DiskStage::kCrit), "crit");
}

class DiskMonitorGapFixture : public ::testing::Test {
protected:
    void SetUp() override { DeployMetrics::Instance().ResetForTest(); }
    void TearDown() override { DeployMetrics::Instance().ResetForTest(); }
};

TEST_F(DiskMonitorGapFixture, CheckOnceRealStatvfsClassifiesAndStores) {
    // Real statvfs on /tmp: success path through CheckOnce -> SampleDiskUsage ->
    // StageFor -> Apply. Use thresholds of 1.0 so any real ratio classifies NORMAL
    // (deterministic regardless of the host's actual disk fill level).
    DiskMonitorConfig cfg;
    cfg.data_dir = "/tmp";
    cfg.warn_threshold = 0.99;
    cfg.crit_threshold = 1.0;
    DiskMonitor mon(cfg);

    DiskUsage s = mon.CheckOnce();
    EXPECT_GT(s.total_bytes, 0u);
    EXPECT_GE(s.usage_ratio, 0.0);
    EXPECT_LE(s.usage_ratio, 1.0);
    EXPECT_EQ(s.stage, DiskStage::kNormal);
    EXPECT_FALSE(mon.ShouldRejectWrites());

    // Usage() reconstructs the same snapshot from the atomics Apply() stored.
    DiskUsage u = mon.Usage();
    EXPECT_EQ(u.total_bytes, s.total_bytes);
    EXPECT_EQ(u.free_bytes, s.free_bytes);
    EXPECT_EQ(u.stage, DiskStage::kNormal);
}

TEST_F(DiskMonitorGapFixture, CheckOnceFailSafeKeepsLastSample) {
    // Seed a known sample via the injection seam, then point at a non-existent
    // path: statvfs fails, CheckOnce must return the *last* sample unchanged and
    // never flip the reject flag (fail-safe, §6.2).
    DiskMonitorConfig cfg;
    cfg.data_dir = "/no/such/path/for/statvfs_xyz_404";
    cfg.warn_threshold = 0.80;
    cfg.crit_threshold = 0.90;
    DiskMonitor mon(cfg);

    DiskUsage seed;
    seed.total_bytes = 1000;
    seed.free_bytes = 700;
    seed.usage_ratio = 0.30;
    mon.CheckWithSample(seed);  // NORMAL committed to the atomics
    ASSERT_FALSE(mon.ShouldRejectWrites());

    DiskUsage after = mon.CheckOnce();  // statvfs fails -> returns Usage()
    EXPECT_EQ(after.total_bytes, 1000u);
    EXPECT_EQ(after.free_bytes, 700u);
    EXPECT_EQ(after.stage, DiskStage::kNormal);
    EXPECT_FALSE(mon.ShouldRejectWrites());  // transient stat error never rejects
}

TEST_F(DiskMonitorGapFixture, UsageRatioZeroWhenTotalZero) {
    // Usage() ratio reconstruction guard: total_bytes == 0 -> ratio 0.0 (no div0).
    DiskMonitorConfig cfg;
    cfg.data_dir = "/tmp";
    DiskMonitor mon(cfg);
    // Fresh monitor: atomics default to 0/0; Usage() must not divide by zero.
    DiskUsage u = mon.Usage();
    EXPECT_EQ(u.total_bytes, 0u);
    EXPECT_DOUBLE_EQ(u.usage_ratio, 0.0);
}

}  // namespace
}  // namespace cortrix::deploy
