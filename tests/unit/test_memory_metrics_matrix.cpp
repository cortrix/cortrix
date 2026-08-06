// Memory metrics family — exhaustive matrix coverage for the memory features
// (enum ToString, label-combo counters, gauges, histograms, render, reset).
// Separate suite namespaces from the existing test_mem0*_metrics.cpp files.
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <tuple>

#include "cortrix/memory/memory_extract_metrics.h"
#include "cortrix/memory/memory_metrics.h"
#include "cortrix/memory/memory_opt_out_metrics.h"
#include "cortrix/memory/memory_isolation_metrics.h"

// ============================== memory extraction ============================
namespace cortrix::memory {
namespace mem02_matrix {

using ES = MemoryExtractMetrics::ExtractStatus;
using TD = MemoryExtractMetrics::TokenDirection;
using CB = MemoryExtractMetrics::ConfidenceBucket;
using TB = MemoryExtractMetrics::TriggeredBy;

class MemoryExtractFx : public ::testing::Test {
protected:
    void SetUp() override { MemoryExtractMetrics::Instance().ResetForTest(); }
    void TearDown() override { MemoryExtractMetrics::Instance().ResetForTest(); }
    MemoryExtractMetrics& m() { return MemoryExtractMetrics::Instance(); }
};

TEST_F(MemoryExtractFx, ToStringAll) {
    EXPECT_STREQ(ToString(ES::kSuccess), "success");
    EXPECT_STREQ(ToString(ES::kFailed), "failed");
    EXPECT_STREQ(ToString(TD::kInput), "input");
    EXPECT_STREQ(ToString(TD::kOutput), "output");
    EXPECT_STREQ(ToString(CB::kHigh), "high");
    EXPECT_STREQ(ToString(CB::kMedium), "medium");
    EXPECT_STREQ(ToString(CB::kLow), "low");
    EXPECT_STREQ(ToString(TB::kLlmAuto), "llm_auto");
    EXPECT_STREQ(ToString(TB::kManual), "manual");
    EXPECT_STREQ(ToString(TB::kAgentSelf), "agent_self");
}

TEST_F(MemoryExtractFx, BucketForConfidenceBoundaries) {
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(1.0), CB::kHigh);
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.8), CB::kHigh);   // [0.8,1.0]
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.79), CB::kMedium);
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.5), CB::kMedium);  // [0.5,0.8)
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.49), CB::kLow);
    EXPECT_EQ(MemoryExtractMetrics::BucketForConfidence(0.0), CB::kLow);     // [0,0.5)
}

class MemoryExtractMatrix : public MemoryExtractFx, public ::testing::WithParamInterface<ES> {};
TEST_P(MemoryExtractMatrix, Increments) {
    m().RecordExtract(GetParam());
    EXPECT_EQ(m().ExtractCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, MemoryExtractMatrix,
                         ::testing::Values(ES::kSuccess, ES::kFailed));

class MemoryExtractConfMatrix : public MemoryExtractFx, public ::testing::WithParamInterface<CB> {};
TEST_P(MemoryExtractConfMatrix, Increments) {
    m().RecordContradiction(GetParam());
    m().RecordContradiction(GetParam());
    EXPECT_EQ(m().ContradictionCount(GetParam()), 2u);
}
INSTANTIATE_TEST_SUITE_P(All, MemoryExtractConfMatrix,
                         ::testing::Values(CB::kHigh, CB::kMedium, CB::kLow));

class MemoryExtractTrigMatrix : public MemoryExtractFx, public ::testing::WithParamInterface<TB> {};
TEST_P(MemoryExtractTrigMatrix, Increments) {
    m().RecordInvalidation(GetParam());
    EXPECT_EQ(m().InvalidationCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, MemoryExtractTrigMatrix,
                         ::testing::Values(TB::kLlmAuto, TB::kManual,
                                           TB::kAgentSelf));

TEST_F(MemoryExtractFx, TokensSummedOverModelsByDirection) {
    m().RecordTokens("gpt", TD::kInput, 10);
    m().RecordTokens("claude", TD::kInput, 5);
    m().RecordTokens("gpt", TD::kOutput, 7);
    EXPECT_EQ(m().TokenCount(TD::kInput), 15u);
    EXPECT_EQ(m().TokenCount(TD::kOutput), 7u);
}

TEST_F(MemoryExtractFx, QueueGaugeLastWriteWins) {
    m().SetQueueDepth(9);
    EXPECT_EQ(m().QueueDepth(), 9);
    m().SetQueueDepth(2);
    EXPECT_EQ(m().QueueDepth(), 2);
}

// extract_duration histogram bounds {0.1,0.25,0.5,1,2,5,10,30}s.
TEST_F(MemoryExtractFx, ExtractDurationHistogramRender) {
    m().ObserveExtractDuration("gpt", 100);    // 0.1s exactly first bound
    m().ObserveExtractDuration("gpt", 50);     // under
    m().ObserveExtractDuration("gpt", 999000);  // above largest -> +Inf
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_memory_extract_duration_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("model=\"gpt\""), std::string::npos);
}

TEST_F(MemoryExtractFx, ExtractDurationNegativeClamps) {
    m().ObserveExtractDuration("m", -5);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("_count{model=\"m\"} 1"), std::string::npos);
}

TEST_F(MemoryExtractFx, RenderHelpTypeAndNoHighCardinality) {
    m().RecordExtract(ES::kSuccess);
    m().RecordTokens("gpt", TD::kInput, 3);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_memory_extract_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_extract_llm_tokens_total counter"),
              std::string::npos);
    EXPECT_EQ(out.find("tenant_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
}

TEST_F(MemoryExtractFx, Reset) {
    m().RecordExtract(ES::kSuccess);
    m().RecordContradiction(CB::kHigh);
    m().RecordInvalidation(TB::kManual);
    m().RecordTokens("gpt", TD::kInput, 3);
    m().SetQueueDepth(4);
    m().ObserveExtractDuration("gpt", 100);
    m().ResetForTest();
    EXPECT_EQ(m().ExtractCount(ES::kSuccess), 0u);
    EXPECT_EQ(m().ContradictionCount(CB::kHigh), 0u);
    EXPECT_EQ(m().InvalidationCount(TB::kManual), 0u);
    EXPECT_EQ(m().TokenCount(TD::kInput), 0u);
    EXPECT_EQ(m().QueueDepth(), 0);
}

}  // namespace mem02_matrix

// ============================== memory opt-out ===============================
namespace immunity {
namespace mem04_matrix {

using TB = MemoryOptOutMetrics::TriggeredBy;

class MemoryOptOutFx : public ::testing::Test {
protected:
    void SetUp() override { MemoryOptOutMetrics::Instance().ResetForTest(); }
    void TearDown() override { MemoryOptOutMetrics::Instance().ResetForTest(); }
    MemoryOptOutMetrics& m() { return MemoryOptOutMetrics::Instance(); }
};

TEST_F(MemoryOptOutFx, ToStringAll) {
    EXPECT_STREQ(ToString(TB::kUser), "user");
    EXPECT_STREQ(ToString(TB::kAgent), "agent");
    EXPECT_STREQ(ToString(TB::kSystem), "system");
}

class MemoryOptOutMatrix : public MemoryOptOutFx, public ::testing::WithParamInterface<TB> {};
TEST_P(MemoryOptOutMatrix, Increments) {
    m().RecordOptOut(GetParam());
    m().RecordOptOut(GetParam());
    EXPECT_EQ(m().OptOutCount(GetParam()), 2u);
    for (TB t : {TB::kUser, TB::kAgent, TB::kSystem})
        if (t != GetParam()) EXPECT_EQ(m().OptOutCount(t), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, MemoryOptOutMatrix,
                         ::testing::Values(TB::kUser, TB::kAgent, TB::kSystem));

TEST_F(MemoryOptOutFx, NoLabelCounters) {
    m().RecordOptOutRevoke();
    m().RecordExtractSkipped();
    m().RecordExtractSkipped();
    EXPECT_EQ(m().OptOutRevokeCount(), 1u);
    EXPECT_EQ(m().ExtractSkippedCount(), 2u);
}

TEST_F(MemoryOptOutFx, RenderHelpTypeNoHighCardinality) {
    m().RecordOptOut(TB::kUser);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_memory_opt_out_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_opt_out_total{triggered_by=\"user\"} 1"),
              std::string::npos);
    EXPECT_EQ(out.find("ns="), std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
}

TEST_F(MemoryOptOutFx, Reset) {
    m().RecordOptOut(TB::kUser);
    m().RecordOptOutRevoke();
    m().RecordExtractSkipped();
    m().ResetForTest();
    EXPECT_EQ(m().OptOutCount(TB::kUser), 0u);
    EXPECT_EQ(m().OptOutRevokeCount(), 0u);
    EXPECT_EQ(m().ExtractSkippedCount(), 0u);
}

}  // namespace mem04_matrix
}  // namespace immunity

// ============================== memory isolation =============================
namespace mem05_matrix {

using CR = MemoryIsolationMetrics::CheckResult;
using AC = MemoryIsolationMetrics::Action;
using RE = MemoryIsolationMetrics::Reason;
using QT = MemoryIsolationMetrics::QuotaType;

constexpr std::array<CR, 2> kResults{CR::kPass, CR::kViolation};
constexpr std::array<AC, 6> kActions{AC::kSearch, AC::kList,    AC::kEdit,
                                     AC::kDelete, AC::kSession, AC::kSessionAccess};
constexpr std::array<RE, 3> kReasons{RE::kMissingUserId, RE::kEmptyUserId,
                                     RE::kMismatch};
constexpr std::array<QT, 3> kQuotas{QT::kItemsCount, QT::kSessionCount,
                                    QT::kMemoryBytes};

class MemoryIsolationFx : public ::testing::Test {
protected:
    void SetUp() override { MemoryIsolationMetrics::Instance().ResetForTest(); }
    void TearDown() override { MemoryIsolationMetrics::Instance().ResetForTest(); }
    MemoryIsolationMetrics& m() { return MemoryIsolationMetrics::Instance(); }
};

TEST_F(MemoryIsolationFx, ToStringAll) {
    EXPECT_STREQ(ToString(CR::kPass), "pass");
    EXPECT_STREQ(ToString(CR::kViolation), "violation");
    EXPECT_STREQ(ToString(AC::kSearch), "search");
    EXPECT_STREQ(ToString(AC::kList), "list");
    EXPECT_STREQ(ToString(AC::kEdit), "edit");
    EXPECT_STREQ(ToString(AC::kDelete), "delete");
    EXPECT_STREQ(ToString(AC::kSession), "session");
    EXPECT_STREQ(ToString(AC::kSessionAccess), "session_access");
    EXPECT_STREQ(ToString(RE::kMissingUserId), "missing_user_id");
    EXPECT_STREQ(ToString(RE::kEmptyUserId), "empty_user_id");
    EXPECT_STREQ(ToString(RE::kMismatch), "mismatch");
    EXPECT_STREQ(ToString(QT::kItemsCount), "items_count");
    EXPECT_STREQ(ToString(QT::kSessionCount), "session_count");
    EXPECT_STREQ(ToString(QT::kMemoryBytes), "memory_bytes");
}

using CheckParam = std::tuple<CR, AC>;
class MemoryIsolationCheckMatrix : public MemoryIsolationFx, public ::testing::WithParamInterface<CheckParam> {};
TEST_P(MemoryIsolationCheckMatrix, IncrementsOnlyTargetCell) {
    auto [r, a] = GetParam();
    m().RecordIsolationCheck(r, a);
    EXPECT_EQ(m().IsolationCheckCount(r, a), 1u);
    for (CR rr : kResults)
        for (AC aa : kActions)
            if (!(rr == r && aa == a))
                EXPECT_EQ(m().IsolationCheckCount(rr, aa), 0u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, MemoryIsolationCheckMatrix,
                         ::testing::Combine(::testing::ValuesIn(kResults),
                                            ::testing::ValuesIn(kActions)));

using ViolParam = std::tuple<AC, RE>;
class MemoryIsolationViolMatrix : public MemoryIsolationFx, public ::testing::WithParamInterface<ViolParam> {};
TEST_P(MemoryIsolationViolMatrix, Increments) {
    auto [a, r] = GetParam();
    m().RecordIsolationViolation(a, r);
    EXPECT_EQ(m().IsolationViolationCount(a, r), 1u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, MemoryIsolationViolMatrix,
                         ::testing::Combine(::testing::ValuesIn(kActions),
                                            ::testing::ValuesIn(kReasons)));

class MemoryIsolationQuotaMatrix : public MemoryIsolationFx, public ::testing::WithParamInterface<QT> {};
TEST_P(MemoryIsolationQuotaMatrix, CounterAndGauge) {
    m().RecordQuotaExceeded(GetParam());
    EXPECT_EQ(m().QuotaExceededCount(GetParam()), 1u);
    m().SetQuotaUsageRatio(GetParam(), 0.75);
    EXPECT_NEAR(m().QuotaUsageRatio(GetParam()), 0.75, 1e-6);
}
INSTANTIATE_TEST_SUITE_P(All, MemoryIsolationQuotaMatrix, ::testing::ValuesIn(kQuotas));

class MemoryIsolationScopeMatrix : public MemoryIsolationFx, public ::testing::WithParamInterface<RE> {};
TEST_P(MemoryIsolationScopeMatrix, Increments) {
    m().RecordMatchScopeExcluded(GetParam());
    EXPECT_EQ(m().MatchScopeExcludedCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, MemoryIsolationScopeMatrix, ::testing::ValuesIn(kReasons));

TEST_F(MemoryIsolationFx, NoLabelGaugeAndCounter) {
    m().SetUserSessionCount(12);
    EXPECT_EQ(m().UserSessionCount(), 12);
    m().RecordDefaultUserUsed();
    m().RecordDefaultUserUsed();
    EXPECT_EQ(m().DefaultUserUsedCount(), 2u);
}

TEST_F(MemoryIsolationFx, RenderHelpTypeNoHighCardinality) {
    m().RecordIsolationCheck(CR::kPass, AC::kSearch);
    m().SetQuotaUsageRatio(QT::kItemsCount, 0.5);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_memory_isolation_check_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_isolation_quota_usage_ratio gauge"),
              std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
}

TEST_F(MemoryIsolationFx, Reset) {
    m().RecordIsolationCheck(CR::kPass, AC::kSearch);
    m().RecordIsolationViolation(AC::kSession, RE::kMismatch);
    m().RecordQuotaExceeded(QT::kItemsCount);
    m().SetQuotaUsageRatio(QT::kItemsCount, 0.9);
    m().SetUserSessionCount(5);
    m().RecordDefaultUserUsed();
    m().RecordMatchScopeExcluded(RE::kMismatch);
    m().ResetForTest();
    EXPECT_EQ(m().IsolationCheckCount(CR::kPass, AC::kSearch), 0u);
    EXPECT_EQ(m().IsolationViolationCount(AC::kSession, RE::kMismatch), 0u);
    EXPECT_EQ(m().QuotaExceededCount(QT::kItemsCount), 0u);
    EXPECT_NEAR(m().QuotaUsageRatio(QT::kItemsCount), 0.0, 1e-9);
    EXPECT_EQ(m().UserSessionCount(), 0);
    EXPECT_EQ(m().DefaultUserUsedCount(), 0u);
    EXPECT_EQ(m().MatchScopeExcludedCount(RE::kMismatch), 0u);
}

}  // namespace mem05_matrix
}  // namespace cortrix::memory

// ============================== memory transparency ==========================
namespace cortrix::memory::transparency {
namespace mem03_matrix {

using Op = MemoryMetrics::Op;
using OS = MemoryMetrics::OpStatus;
using EC = MemoryMetrics::ErrorCodeLabel;

constexpr std::array<Op, 4> kOps{Op::kList, Op::kCreate, Op::kEdit,
                                 Op::kInvalidate};
constexpr std::array<OS, 2> kStatuses{OS::kSuccess, OS::kError};
constexpr std::array<EC, 5> kErrs{EC::kMemoryNotFound, EC::kUserMismatch,
                                  EC::kAlreadyInvalidated, EC::kInvalidateFailed,
                                  EC::kQuota};

class MemoryFx : public ::testing::Test {
protected:
    void SetUp() override { MemoryMetrics::Instance().ResetForTest(); }
    void TearDown() override { MemoryMetrics::Instance().ResetForTest(); }
    MemoryMetrics& m() { return MemoryMetrics::Instance(); }
};

TEST_F(MemoryFx, ToStringAll) {
    EXPECT_STREQ(ToString(Op::kList), "list");
    EXPECT_STREQ(ToString(Op::kCreate), "create");
    EXPECT_STREQ(ToString(Op::kEdit), "edit");
    EXPECT_STREQ(ToString(Op::kInvalidate), "invalidate");
    EXPECT_STREQ(ToString(OS::kSuccess), "success");
    EXPECT_STREQ(ToString(OS::kError), "error");
    EXPECT_STREQ(ToString(EC::kMemoryNotFound), "memory_not_found");
    EXPECT_STREQ(ToString(EC::kUserMismatch), "user_mismatch");
    EXPECT_STREQ(ToString(EC::kAlreadyInvalidated), "already_invalidated");
    EXPECT_STREQ(ToString(EC::kInvalidateFailed), "invalidate_failed");
    EXPECT_STREQ(ToString(EC::kQuota), "quota");
}

using OpParam = std::tuple<Op, OS>;
class MemoryOpMatrix : public MemoryFx, public ::testing::WithParamInterface<OpParam> {};
TEST_P(MemoryOpMatrix, IncrementsOnlyTargetCell) {
    auto [op, st] = GetParam();
    m().RecordOp(op, st);
    EXPECT_EQ(m().OpCount(op, st), 1u);
    for (Op o : kOps)
        for (OS s : kStatuses)
            if (!(o == op && s == st)) EXPECT_EQ(m().OpCount(o, s), 0u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, MemoryOpMatrix,
                         ::testing::Combine(::testing::ValuesIn(kOps),
                                            ::testing::ValuesIn(kStatuses)));

class MemoryErrMatrix : public MemoryFx, public ::testing::WithParamInterface<EC> {};
TEST_P(MemoryErrMatrix, Increments) {
    m().RecordInvalidInput(GetParam());
    EXPECT_EQ(m().InvalidInputCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, MemoryErrMatrix, ::testing::ValuesIn(kErrs));

TEST_F(MemoryFx, NoLabelCounters) {
    m().RecordCrossUserBlocked();
    m().RecordCrossUserBlocked();
    m().RecordEditConflict();
    EXPECT_EQ(m().CrossUserBlockedCount(), 2u);
    EXPECT_EQ(m().EditConflictCount(), 1u);
}

// op_latency histogram per op, bounds {0.005,...,1}s.
TEST_F(MemoryFx, OpLatencyHistogramRender) {
    m().ObserveOpLatency(Op::kList, 5);    // 0.005s exactly first bound
    m().ObserveOpLatency(Op::kList, 2);    // under
    m().ObserveOpLatency(Op::kList, 99999);  // above largest -> +Inf
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_memory_transparency_op_latency_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("op=\"list\""), std::string::npos);
}

TEST_F(MemoryFx, OpLatencyNegativeClamps) {
    m().ObserveOpLatency(Op::kEdit, -3);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("op=\"edit\""), std::string::npos);
}

TEST_F(MemoryFx, RenderHelpTypeNoHighCardinality) {
    m().RecordOp(Op::kList, OS::kSuccess);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_memory_transparency_op_total counter"),
              std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(MemoryFx, Reset) {
    m().RecordOp(Op::kList, OS::kSuccess);
    m().RecordInvalidInput(EC::kQuota);
    m().RecordCrossUserBlocked();
    m().RecordEditConflict();
    m().ObserveOpLatency(Op::kList, 5);
    m().ResetForTest();
    EXPECT_EQ(m().OpCount(Op::kList, OS::kSuccess), 0u);
    EXPECT_EQ(m().InvalidInputCount(EC::kQuota), 0u);
    EXPECT_EQ(m().CrossUserBlockedCount(), 0u);
    EXPECT_EQ(m().EditConflictCount(), 0u);
}

}  // namespace mem03_matrix
}  // namespace cortrix::memory::transparency
