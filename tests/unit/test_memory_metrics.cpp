#include <gtest/gtest.h>

#include <string>

#include "cortrix/memory/memory_metrics.h"

// S6 coverage: the 5 cortrix_memory_transparency_* metrics — counters +
// per-op latency histogram recording + the OpenMetrics renderer + label-enum
// discipline (OBS_SPEC no high-cardinality labels).
namespace cortrix::memory::transparency {
namespace {

using Op = MemoryMetrics::Op;
using OpStatus = MemoryMetrics::OpStatus;
using ErrorCodeLabel = MemoryMetrics::ErrorCodeLabel;

class MemoryMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { MemoryMetrics::Instance().ResetForTest(); }
    void TearDown() override { MemoryMetrics::Instance().ResetForTest(); }
    MemoryMetrics& M() { return MemoryMetrics::Instance(); }
};

TEST_F(MemoryMetricsTest, OpTotalCountsByOpAndStatus) {
    M().RecordOp(Op::kList, OpStatus::kSuccess);
    M().RecordOp(Op::kList, OpStatus::kSuccess);
    M().RecordOp(Op::kList, OpStatus::kError);
    M().RecordOp(Op::kInvalidate, OpStatus::kSuccess);
    EXPECT_EQ(M().OpCount(Op::kList, OpStatus::kSuccess), 2u);
    EXPECT_EQ(M().OpCount(Op::kList, OpStatus::kError), 1u);
    EXPECT_EQ(M().OpCount(Op::kInvalidate, OpStatus::kSuccess), 1u);
    EXPECT_EQ(M().OpCount(Op::kCreate, OpStatus::kSuccess), 0u);
    EXPECT_EQ(M().OpCount(Op::kEdit, OpStatus::kError), 0u);
}

TEST_F(MemoryMetricsTest, CrossUserBlockedCounter) {
    EXPECT_EQ(M().CrossUserBlockedCount(), 0u);
    M().RecordCrossUserBlocked();
    M().RecordCrossUserBlocked();
    EXPECT_EQ(M().CrossUserBlockedCount(), 2u);
}

TEST_F(MemoryMetricsTest, EditConflictCounter) {
    M().RecordEditConflict();
    EXPECT_EQ(M().EditConflictCount(), 1u);
}

TEST_F(MemoryMetricsTest, InvalidInputByErrorCode) {
    M().RecordInvalidInput(ErrorCodeLabel::kMemoryNotFound);
    M().RecordInvalidInput(ErrorCodeLabel::kMemoryNotFound);
    M().RecordInvalidInput(ErrorCodeLabel::kQuota);
    EXPECT_EQ(M().InvalidInputCount(ErrorCodeLabel::kMemoryNotFound), 2u);
    EXPECT_EQ(M().InvalidInputCount(ErrorCodeLabel::kQuota), 1u);
    EXPECT_EQ(M().InvalidInputCount(ErrorCodeLabel::kUserMismatch), 0u);
}

TEST_F(MemoryMetricsTest, LatencyNegativeClampedToZero) {
    M().ObserveOpLatency(Op::kList, -5);  // clamped to 0 → falls in the smallest bucket
    std::string out = M().RenderOpenMetrics();
    EXPECT_NE(out.find("cortrix_memory_transparency_op_latency_seconds_count{op=\"list\"} 1"),
              std::string::npos);
}

TEST_F(MemoryMetricsTest, RenderOpenMetricsHasAllFiveMetricsAndTypes) {
    M().RecordOp(Op::kList, OpStatus::kSuccess);
    M().ObserveOpLatency(Op::kList, 12);
    M().RecordCrossUserBlocked();
    M().RecordEditConflict();
    M().RecordInvalidInput(ErrorCodeLabel::kMemoryNotFound);

    std::string out = M().RenderOpenMetrics();
    // All 5 metric names present.
    EXPECT_NE(out.find("cortrix_memory_transparency_op_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_transparency_op_latency_seconds"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_transparency_cross_user_blocked_total"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_transparency_edit_conflict_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_transparency_invalid_input_total"), std::string::npos);
    // TYPE lines present.
    EXPECT_NE(out.find("# TYPE cortrix_memory_transparency_op_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_transparency_op_latency_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_transparency_cross_user_blocked_total counter"),
              std::string::npos);
    // Histogram MUST render cumulative _bucket{le=...} incl +Inf + _sum + _count.
    EXPECT_NE(out.find("cortrix_memory_transparency_op_latency_seconds_bucket{"),
              std::string::npos);
    EXPECT_NE(out.find("le=\"+Inf\""), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_transparency_op_latency_seconds_sum{"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_transparency_op_latency_seconds_count{"),
              std::string::npos);
}

// The latency histogram renders well-formed cumulative buckets per op: every declared
// le bound + a +Inf bucket, monotonically non-decreasing, with +Inf == _count.
TEST_F(MemoryMetricsTest, OpLatencyHistogramBucketsAreCumulativeAndComplete) {
    // Observations spanning several buckets for op=edit (ms): 3→0.003s (le 0.005),
    // 20→0.02s (le 0.025), 200→0.2s (le 0.25), 5000→5s (+Inf only).
    M().ObserveOpLatency(Op::kEdit, 3);
    M().ObserveOpLatency(Op::kEdit, 20);
    M().ObserveOpLatency(Op::kEdit, 200);
    M().ObserveOpLatency(Op::kEdit, 5000);
    std::string out = M().RenderOpenMetrics();

    const std::string prefix =
        "cortrix_memory_transparency_op_latency_seconds_bucket{op=\"edit\",le=\"";
    auto bucket_value = [&](const std::string& le) -> long {
        std::string key = prefix + le + "\"} ";
        size_t p = out.find(key);
        if (p == std::string::npos) return -1;
        p += key.size();
        return std::stol(out.substr(p, out.find('\n', p) - p));
    };

    long prev = 0;
    for (const char* le : {"0.005", "0.01", "0.025", "0.05", "0.1", "0.25", "0.5", "1"}) {
        long v = bucket_value(le);
        ASSERT_GE(v, 0) << "missing bucket le=" << le;
        EXPECT_GE(v, prev) << "bucket le=" << le << " not cumulative";
        prev = v;
    }
    EXPECT_EQ(bucket_value("0.005"), 1);  // 0.003s
    EXPECT_EQ(bucket_value("0.025"), 2);  // + 0.02
    EXPECT_EQ(bucket_value("0.25"), 3);   // + 0.2
    EXPECT_EQ(bucket_value("1"), 3);      // 5s only in +Inf
    EXPECT_EQ(bucket_value("+Inf"), 4);   // +Inf == total observations
    EXPECT_NE(out.find("cortrix_memory_transparency_op_latency_seconds_count{op=\"edit\"} 4"),
              std::string::npos);
}

TEST_F(MemoryMetricsTest, RenderHasNoHighCardinalityLabels) {
    // OBS_SPEC / memory transparency: labels are enum-only. No tenant_id / ns_id / user_id.
    M().RecordOp(Op::kList, OpStatus::kSuccess);
    M().ObserveOpLatency(Op::kList, 10);
    M().RecordInvalidInput(ErrorCodeLabel::kUserMismatch);
    std::string out = M().RenderOpenMetrics();
    EXPECT_EQ(out.find("tenant_id"), std::string::npos);
    EXPECT_EQ(out.find("ns_id"), std::string::npos);
    EXPECT_EQ(out.find("user_id"), std::string::npos);
    // The op label values are the 4 expected ones; status only success/error.
    EXPECT_NE(out.find("op=\"list\""), std::string::npos);
    EXPECT_NE(out.find("op=\"invalidate\""), std::string::npos);
}

TEST_F(MemoryMetricsTest, LabelStringsMatchSpec) {
    EXPECT_STREQ(ToString(Op::kList), "list");
    EXPECT_STREQ(ToString(Op::kCreate), "create");
    EXPECT_STREQ(ToString(Op::kEdit), "edit");
    EXPECT_STREQ(ToString(Op::kInvalidate), "invalidate");
    EXPECT_STREQ(ToString(OpStatus::kSuccess), "success");
    EXPECT_STREQ(ToString(OpStatus::kError), "error");
    EXPECT_STREQ(ToString(ErrorCodeLabel::kMemoryNotFound), "memory_not_found");
    EXPECT_STREQ(ToString(ErrorCodeLabel::kUserMismatch), "user_mismatch");
    EXPECT_STREQ(ToString(ErrorCodeLabel::kAlreadyInvalidated), "already_invalidated");
    EXPECT_STREQ(ToString(ErrorCodeLabel::kInvalidateFailed), "invalidate_failed");
    EXPECT_STREQ(ToString(ErrorCodeLabel::kQuota), "quota");
}

}  // namespace
}  // namespace cortrix::memory::transparency
