#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/deploy/deploy_metrics.h"
#include "cortrix/deploy/disk_monitor.h"
#include "cortrix/deploy/deploy_error.h"

// Deployment coverage: the disk-space management model — the 4 CX_ERR_* identities
// (§12 registry table + structured_data keys + AgentFriendlyError builder), the
// DiskMonitor NORMAL/WARN/CRIT FSM + write-rejection flag (§6.1),
// the IGlobalConfig threshold loader (§6.4), and the disk_usage_ratio gauge feed.
namespace cortrix::deploy {
namespace {

using agent_friendly::ErrorCategory;

// ------------------------- deployment error registry (§12) ------------------

constexpr DeployErrorCode kAll[] = {
    DeployErrorCode::kDiskFull,
    DeployErrorCode::kShutdownInProgress,
    DeployErrorCode::kHealthCheckFailed,
    DeployErrorCode::kBfNotReady,
};

TEST(DeployErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kDeployErrorCodeCount, 4);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kDeployErrorCodeCount));
}

TEST(DeployErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (DeployErrorCode c : kAll) {
        std::string s = DeployErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_", 0), 0u) << s << " must start with CX_ERR_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 4u);
}

TEST(DeployErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](DeployErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry) {
        const DeployErrorInfo& i = GetDeployErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
    };
    chk(DeployErrorCode::kDiskFull, "CX_ERR_DISK_FULL", 507,
        ErrorCategory::kPermanent, false);
    chk(DeployErrorCode::kShutdownInProgress, "CX_ERR_SHUTDOWN_IN_PROGRESS", 503,
        ErrorCategory::kTransient, true);
    chk(DeployErrorCode::kHealthCheckFailed, "CX_ERR_HEALTH_CHECK_FAILED", 503,
        ErrorCategory::kTransient, true);
    chk(DeployErrorCode::kBfNotReady, "CX_ERR_BF_NOT_READY", 503,
        ErrorCategory::kTransient, true);
    // Non-retryable code carries no retry_after_ms; retryable ones do.
    EXPECT_FALSE(GetDeployErrorInfo(DeployErrorCode::kDiskFull).retry_after_ms.has_value());
    EXPECT_TRUE(GetDeployErrorInfo(DeployErrorCode::kShutdownInProgress).retry_after_ms.has_value());
}

TEST(DeployErrorTest, DiskFullRequiresFiveStructuredKeys) {
    const auto& keys = RequiredStructuredDataKeys(DeployErrorCode::kDiskFull);
    EXPECT_EQ(keys.size(), 5u);  // §6.3
    nlohmann::json complete = {
        {"disk_usage_ratio", 0.93}, {"free_bytes", 1024}, {"data_path", "/cortrix-data"},
        {"warn_threshold", 0.8}, {"crit_threshold", 0.9}};
    EXPECT_TRUE(HasRequiredStructuredData(DeployErrorCode::kDiskFull, complete));
    nlohmann::json missing = {{"disk_usage_ratio", 0.93}};
    EXPECT_FALSE(HasRequiredStructuredData(DeployErrorCode::kDiskFull, missing));
}

TEST(DeployErrorTest, MakeErrorFillsCanonicalFields) {
    auto err = MakeDeployError(DeployErrorCode::kBfNotReady, {{"rebuild_progress", 0.4}});
    EXPECT_EQ(err.code, "CX_ERR_BF_NOT_READY");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 2000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["rebuild_progress"], 0.4);
}

TEST(DeployErrorTest, StatusBridgePreservesCodeToken) {
    Status s = DeployStatus(DeployErrorCode::kDiskFull, "no space");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    // CX_ERR_* token recoverable in the message for boundary re-inflation.
    EXPECT_NE(s.message().find("CX_ERR_DISK_FULL"), std::string::npos);
    EXPECT_NE(s.message().find("no space"), std::string::npos);
}

// ------------------------- DiskMonitor FSM (§6.1) -------------------------

DiskUsage RawAtRatio(double ratio) {
    DiskUsage u;
    u.total_bytes = 1000;
    u.free_bytes = static_cast<uint64_t>(1000.0 * (1.0 - ratio));
    u.usage_ratio = ratio;
    return u;
}

class DiskMonitorTest : public ::testing::Test {
protected:
    void SetUp() override { DeployMetrics::Instance().ResetForTest(); }
    void TearDown() override { DeployMetrics::Instance().ResetForTest(); }
};

TEST_F(DiskMonitorTest, ClassifiesThreeStagesAtDefaults) {
    DiskMonitorConfig cfg;  // warn 0.80, crit 0.90
    cfg.data_dir = "/tmp";
    DiskMonitor mon(cfg);

    EXPECT_EQ(mon.CheckWithSample(RawAtRatio(0.50)).stage, DiskStage::kNormal);
    EXPECT_FALSE(mon.ShouldRejectWrites());

    EXPECT_EQ(mon.CheckWithSample(RawAtRatio(0.85)).stage, DiskStage::kWarn);
    EXPECT_FALSE(mon.ShouldRejectWrites());  // WARN still allows writes

    EXPECT_EQ(mon.CheckWithSample(RawAtRatio(0.93)).stage, DiskStage::kCrit);
    EXPECT_TRUE(mon.ShouldRejectWrites());  // CRIT rejects writes
}

TEST_F(DiskMonitorTest, BoundaryRatiosAreInclusive) {
    DiskMonitorConfig cfg;
    cfg.data_dir = "/tmp";
    DiskMonitor mon(cfg);
    // exactly at warn → WARN; exactly at crit → CRIT (>= semantics, §6.1).
    EXPECT_EQ(mon.CheckWithSample(RawAtRatio(0.80)).stage, DiskStage::kWarn);
    EXPECT_EQ(mon.CheckWithSample(RawAtRatio(0.90)).stage, DiskStage::kCrit);
    // dropping back below warn clears the reject flag (recovery).
    EXPECT_EQ(mon.CheckWithSample(RawAtRatio(0.10)).stage, DiskStage::kNormal);
    EXPECT_FALSE(mon.ShouldRejectWrites());
}

TEST_F(DiskMonitorTest, FeedsDiskUsageRatioGauge) {
    DiskMonitorConfig cfg;
    cfg.data_dir = "/tmp";
    DiskMonitor mon(cfg);
    mon.CheckWithSample(RawAtRatio(0.77));
    EXPECT_DOUBLE_EQ(DeployMetrics::Instance().DiskUsageRatio(), 0.77);
}

TEST_F(DiskMonitorTest, StageChangeCallbackFiresOnlyOnTransition) {
    int calls = 0;
    DiskStage last = DiskStage::kNormal;
    DiskMonitorConfig cfg;
    cfg.data_dir = "/tmp";
    DiskMonitor mon(cfg, [&](const DiskUsage& u) { ++calls; last = u.stage; });

    mon.CheckWithSample(RawAtRatio(0.50));  // NORMAL (start state) → no change
    mon.CheckWithSample(RawAtRatio(0.55));  // still NORMAL → no callback
    EXPECT_EQ(calls, 0);
    mon.CheckWithSample(RawAtRatio(0.85));  // → WARN (1)
    mon.CheckWithSample(RawAtRatio(0.93));  // → CRIT (2)
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(last, DiskStage::kCrit);
}

TEST_F(DiskMonitorTest, MakeDiskFullErrorHasCompleteBody) {
    DiskMonitorConfig cfg;
    cfg.data_dir = "/cortrix-data";
    cfg.warn_threshold = 0.8;
    cfg.crit_threshold = 0.9;
    DiskMonitor mon(cfg);
    mon.CheckWithSample(RawAtRatio(0.95));
    auto err = mon.MakeDiskFullError();
    EXPECT_EQ(err.code, "CX_ERR_DISK_FULL");
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_TRUE(HasRequiredStructuredData(DeployErrorCode::kDiskFull, *err.structured_data));
    EXPECT_EQ((*err.structured_data)["data_path"], "/cortrix-data");
    EXPECT_DOUBLE_EQ((*err.structured_data)["crit_threshold"].get<double>(), 0.9);
}

// ------------------------- threshold loader (§6.4) -------------------------

TEST(DiskConfigTest, DefaultsWhenConfigNull) {
    auto c = DiskMonitorConfig::FromGlobalConfig(nullptr, "/data");
    EXPECT_EQ(c.data_dir, "/data");
    EXPECT_DOUBLE_EQ(c.warn_threshold, 0.80);
    EXPECT_DOUBLE_EQ(c.crit_threshold, 0.90);
    EXPECT_EQ(c.check_interval_sec, 30);
}

TEST(DiskConfigTest, ReadsKeysFromGlobalConfig) {
    InMemoryGlobalConfig cfg;
    cfg.Set(DiskMonitorConfig::kWarnKey, "0.70");
    cfg.Set(DiskMonitorConfig::kCritKey, "0.85");
    cfg.Set(DiskMonitorConfig::kIntervalKey, "60");
    auto c = DiskMonitorConfig::FromGlobalConfig(&cfg, "/data");
    // IGlobalConfig::GetFloat returns a 32-bit float, so compare with a float
    // epsilon rather than DOUBLE_EQ (0.70f != 0.70 at double precision).
    EXPECT_NEAR(c.warn_threshold, 0.70, 1e-6);
    EXPECT_NEAR(c.crit_threshold, 0.85, 1e-6);
    EXPECT_EQ(c.check_interval_sec, 60);
}

TEST(DiskConfigTest, ClampsOutOfRangeAndFixesInversion) {
    InMemoryGlobalConfig cfg;
    cfg.Set(DiskMonitorConfig::kWarnKey, "0.99");   // > 0.95 → clamp to 0.95
    cfg.Set(DiskMonitorConfig::kCritKey, "0.50");   // < 0.6  → clamp to 0.6
    cfg.Set(DiskMonitorConfig::kIntervalKey, "5");  // < 10   → clamp to 10
    auto c = DiskMonitorConfig::FromGlobalConfig(&cfg, "/data");
    EXPECT_EQ(c.check_interval_sec, 10);
    EXPECT_DOUBLE_EQ(c.crit_threshold, 0.60);
    // warn (clamped 0.95) >= crit (0.60) → inversion fix pulls warn below crit.
    EXPECT_LT(c.warn_threshold, c.crit_threshold);
}

TEST(DiskConfigTest, RealStatvfsSampleSucceedsOnTmp) {
    // SampleDiskUsage against a real path returns a plausible ratio in [0,1].
    DiskUsage u;
    ASSERT_TRUE(SampleDiskUsage("/tmp", &u));
    EXPECT_GT(u.total_bytes, 0u);
    EXPECT_GE(u.usage_ratio, 0.0);
    EXPECT_LE(u.usage_ratio, 1.0);
}

}  // namespace
}  // namespace cortrix::deploy
