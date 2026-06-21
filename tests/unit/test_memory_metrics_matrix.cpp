// Memory metrics family — exhaustive matrix coverage for MEM02/03/04/05
// (enum ToString, label-combo counters, gauges, histograms, render, reset).
// Separate suite namespaces from the existing test_mem0*_metrics.cpp files.
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <tuple>

#include "cortrix/memory/mem02_metrics.h"
#include "cortrix/memory/mem03_metrics.h"
#include "cortrix/memory/mem04_metrics.h"
#include "cortrix/memory/mem05_metrics.h"

// ============================== MEM02 ========================================
namespace cortrix::memory {
namespace mem02_matrix {

using ES = Mem02Metrics::ExtractStatus;
using TD = Mem02Metrics::TokenDirection;
using CB = Mem02Metrics::ConfidenceBucket;
using TB = Mem02Metrics::TriggeredBy;

class Mem02Fx : public ::testing::Test {
protected:
    void SetUp() override { Mem02Metrics::Instance().ResetForTest(); }
    void TearDown() override { Mem02Metrics::Instance().ResetForTest(); }
    Mem02Metrics& m() { return Mem02Metrics::Instance(); }
};

TEST_F(Mem02Fx, ToStringAll) {
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

TEST_F(Mem02Fx, BucketForConfidenceBoundaries) {
    EXPECT_EQ(Mem02Metrics::BucketForConfidence(1.0), CB::kHigh);
    EXPECT_EQ(Mem02Metrics::BucketForConfidence(0.8), CB::kHigh);   // [0.8,1.0]
    EXPECT_EQ(Mem02Metrics::BucketForConfidence(0.79), CB::kMedium);
    EXPECT_EQ(Mem02Metrics::BucketForConfidence(0.5), CB::kMedium);  // [0.5,0.8)
    EXPECT_EQ(Mem02Metrics::BucketForConfidence(0.49), CB::kLow);
    EXPECT_EQ(Mem02Metrics::BucketForConfidence(0.0), CB::kLow);     // [0,0.5)
}

class Mem02ExtractMatrix : public Mem02Fx, public ::testing::WithParamInterface<ES> {};
TEST_P(Mem02ExtractMatrix, Increments) {
    m().RecordExtract(GetParam());
    EXPECT_EQ(m().ExtractCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, Mem02ExtractMatrix,
                         ::testing::Values(ES::kSuccess, ES::kFailed));

class Mem02ConfMatrix : public Mem02Fx, public ::testing::WithParamInterface<CB> {};
TEST_P(Mem02ConfMatrix, Increments) {
    m().RecordContradiction(GetParam());
    m().RecordContradiction(GetParam());
    EXPECT_EQ(m().ContradictionCount(GetParam()), 2u);
}
INSTANTIATE_TEST_SUITE_P(All, Mem02ConfMatrix,
                         ::testing::Values(CB::kHigh, CB::kMedium, CB::kLow));

class Mem02TrigMatrix : public Mem02Fx, public ::testing::WithParamInterface<TB> {};
TEST_P(Mem02TrigMatrix, Increments) {
    m().RecordInvalidation(GetParam());
    EXPECT_EQ(m().InvalidationCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, Mem02TrigMatrix,
                         ::testing::Values(TB::kLlmAuto, TB::kManual,
                                           TB::kAgentSelf));

TEST_F(Mem02Fx, TokensSummedOverModelsByDirection) {
    m().RecordTokens("gpt", TD::kInput, 10);
    m().RecordTokens("claude", TD::kInput, 5);
    m().RecordTokens("gpt", TD::kOutput, 7);
    EXPECT_EQ(m().TokenCount(TD::kInput), 15u);
    EXPECT_EQ(m().TokenCount(TD::kOutput), 7u);
}

TEST_F(Mem02Fx, QueueGaugeLastWriteWins) {
    m().SetQueueDepth(9);
    EXPECT_EQ(m().QueueDepth(), 9);
    m().SetQueueDepth(2);
    EXPECT_EQ(m().QueueDepth(), 2);
}

// extract_duration histogram bounds {0.1,0.25,0.5,1,2,5,10,30}s.
TEST_F(Mem02Fx, ExtractDurationHistogramRender) {
    m().ObserveExtractDuration("gpt", 100);    // 0.1s exactly first bound
    m().ObserveExtractDuration("gpt", 50);     // under
    m().ObserveExtractDuration("gpt", 999000);  // above largest -> +Inf
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_mem02_extract_duration_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("model=\"gpt\""), std::string::npos);
}

TEST_F(Mem02Fx, ExtractDurationNegativeClamps) {
    m().ObserveExtractDuration("m", -5);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("_count{model=\"m\"} 1"), std::string::npos);
}

TEST_F(Mem02Fx, RenderHelpTypeAndNoHighCardinality) {
    m().RecordExtract(ES::kSuccess);
    m().RecordTokens("gpt", TD::kInput, 3);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_mem02_extract_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_mem02_llm_tokens_total counter"),
              std::string::npos);
    EXPECT_EQ(out.find("tenant_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
}

TEST_F(Mem02Fx, Reset) {
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

// ============================== MEM04 ========================================
namespace immunity {
namespace mem04_matrix {

using TB = Mem04Metrics::TriggeredBy;

class Mem04Fx : public ::testing::Test {
protected:
    void SetUp() override { Mem04Metrics::Instance().ResetForTest(); }
    void TearDown() override { Mem04Metrics::Instance().ResetForTest(); }
    Mem04Metrics& m() { return Mem04Metrics::Instance(); }
};

TEST_F(Mem04Fx, ToStringAll) {
    EXPECT_STREQ(ToString(TB::kUser), "user");
    EXPECT_STREQ(ToString(TB::kAgent), "agent");
    EXPECT_STREQ(ToString(TB::kSystem), "system");
}

class Mem04OptOutMatrix : public Mem04Fx, public ::testing::WithParamInterface<TB> {};
TEST_P(Mem04OptOutMatrix, Increments) {
    m().RecordOptOut(GetParam());
    m().RecordOptOut(GetParam());
    EXPECT_EQ(m().OptOutCount(GetParam()), 2u);
    for (TB t : {TB::kUser, TB::kAgent, TB::kSystem})
        if (t != GetParam()) EXPECT_EQ(m().OptOutCount(t), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, Mem04OptOutMatrix,
                         ::testing::Values(TB::kUser, TB::kAgent, TB::kSystem));

TEST_F(Mem04Fx, NoLabelCounters) {
    m().RecordOptOutRevoke();
    m().RecordExtractSkipped();
    m().RecordExtractSkipped();
    EXPECT_EQ(m().OptOutRevokeCount(), 1u);
    EXPECT_EQ(m().ExtractSkippedCount(), 2u);
}

TEST_F(Mem04Fx, RenderHelpTypeNoHighCardinality) {
    m().RecordOptOut(TB::kUser);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_mem04_opt_out_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_mem04_opt_out_total{triggered_by=\"user\"} 1"),
              std::string::npos);
    EXPECT_EQ(out.find("ns="), std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
}

TEST_F(Mem04Fx, Reset) {
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

// ============================== MEM05 ========================================
namespace mem05_matrix {

using CR = Mem05Metrics::CheckResult;
using AC = Mem05Metrics::Action;
using RE = Mem05Metrics::Reason;
using QT = Mem05Metrics::QuotaType;

constexpr std::array<CR, 2> kResults{CR::kPass, CR::kViolation};
constexpr std::array<AC, 6> kActions{AC::kSearch, AC::kList,    AC::kEdit,
                                     AC::kDelete, AC::kSession, AC::kSessionAccess};
constexpr std::array<RE, 3> kReasons{RE::kMissingUserId, RE::kEmptyUserId,
                                     RE::kMismatch};
constexpr std::array<QT, 3> kQuotas{QT::kItemsCount, QT::kSessionCount,
                                    QT::kMemoryBytes};

class Mem05Fx : public ::testing::Test {
protected:
    void SetUp() override { Mem05Metrics::Instance().ResetForTest(); }
    void TearDown() override { Mem05Metrics::Instance().ResetForTest(); }
    Mem05Metrics& m() { return Mem05Metrics::Instance(); }
};

TEST_F(Mem05Fx, ToStringAll) {
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
class Mem05CheckMatrix : public Mem05Fx, public ::testing::WithParamInterface<CheckParam> {};
TEST_P(Mem05CheckMatrix, IncrementsOnlyTargetCell) {
    auto [r, a] = GetParam();
    m().RecordIsolationCheck(r, a);
    EXPECT_EQ(m().IsolationCheckCount(r, a), 1u);
    for (CR rr : kResults)
        for (AC aa : kActions)
            if (!(rr == r && aa == a))
                EXPECT_EQ(m().IsolationCheckCount(rr, aa), 0u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, Mem05CheckMatrix,
                         ::testing::Combine(::testing::ValuesIn(kResults),
                                            ::testing::ValuesIn(kActions)));

using ViolParam = std::tuple<AC, RE>;
class Mem05ViolMatrix : public Mem05Fx, public ::testing::WithParamInterface<ViolParam> {};
TEST_P(Mem05ViolMatrix, Increments) {
    auto [a, r] = GetParam();
    m().RecordIsolationViolation(a, r);
    EXPECT_EQ(m().IsolationViolationCount(a, r), 1u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, Mem05ViolMatrix,
                         ::testing::Combine(::testing::ValuesIn(kActions),
                                            ::testing::ValuesIn(kReasons)));

class Mem05QuotaMatrix : public Mem05Fx, public ::testing::WithParamInterface<QT> {};
TEST_P(Mem05QuotaMatrix, CounterAndGauge) {
    m().RecordQuotaExceeded(GetParam());
    EXPECT_EQ(m().QuotaExceededCount(GetParam()), 1u);
    m().SetQuotaUsageRatio(GetParam(), 0.75);
    EXPECT_NEAR(m().QuotaUsageRatio(GetParam()), 0.75, 1e-6);
}
INSTANTIATE_TEST_SUITE_P(All, Mem05QuotaMatrix, ::testing::ValuesIn(kQuotas));

class Mem05ScopeMatrix : public Mem05Fx, public ::testing::WithParamInterface<RE> {};
TEST_P(Mem05ScopeMatrix, Increments) {
    m().RecordMatchScopeExcluded(GetParam());
    EXPECT_EQ(m().MatchScopeExcludedCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, Mem05ScopeMatrix, ::testing::ValuesIn(kReasons));

TEST_F(Mem05Fx, NoLabelGaugeAndCounter) {
    m().SetUserSessionCount(12);
    EXPECT_EQ(m().UserSessionCount(), 12);
    m().RecordDefaultUserUsed();
    m().RecordDefaultUserUsed();
    EXPECT_EQ(m().DefaultUserUsedCount(), 2u);
}

TEST_F(Mem05Fx, RenderHelpTypeNoHighCardinality) {
    m().RecordIsolationCheck(CR::kPass, AC::kSearch);
    m().SetQuotaUsageRatio(QT::kItemsCount, 0.5);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_mem05_isolation_check_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_mem05_quota_usage_ratio gauge"),
              std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
}

TEST_F(Mem05Fx, Reset) {
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

// ============================== MEM03 ========================================
namespace cortrix::memory::transparency {
namespace mem03_matrix {

using Op = Mem03Metrics::Op;
using OS = Mem03Metrics::OpStatus;
using EC = Mem03Metrics::ErrorCodeLabel;

constexpr std::array<Op, 4> kOps{Op::kList, Op::kCreate, Op::kEdit,
                                 Op::kInvalidate};
constexpr std::array<OS, 2> kStatuses{OS::kSuccess, OS::kError};
constexpr std::array<EC, 5> kErrs{EC::kMemoryNotFound, EC::kUserMismatch,
                                  EC::kAlreadyInvalidated, EC::kInvalidateFailed,
                                  EC::kQuota};

class Mem03Fx : public ::testing::Test {
protected:
    void SetUp() override { Mem03Metrics::Instance().ResetForTest(); }
    void TearDown() override { Mem03Metrics::Instance().ResetForTest(); }
    Mem03Metrics& m() { return Mem03Metrics::Instance(); }
};

TEST_F(Mem03Fx, ToStringAll) {
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
class Mem03OpMatrix : public Mem03Fx, public ::testing::WithParamInterface<OpParam> {};
TEST_P(Mem03OpMatrix, IncrementsOnlyTargetCell) {
    auto [op, st] = GetParam();
    m().RecordOp(op, st);
    EXPECT_EQ(m().OpCount(op, st), 1u);
    for (Op o : kOps)
        for (OS s : kStatuses)
            if (!(o == op && s == st)) EXPECT_EQ(m().OpCount(o, s), 0u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, Mem03OpMatrix,
                         ::testing::Combine(::testing::ValuesIn(kOps),
                                            ::testing::ValuesIn(kStatuses)));

class Mem03ErrMatrix : public Mem03Fx, public ::testing::WithParamInterface<EC> {};
TEST_P(Mem03ErrMatrix, Increments) {
    m().RecordInvalidInput(GetParam());
    EXPECT_EQ(m().InvalidInputCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, Mem03ErrMatrix, ::testing::ValuesIn(kErrs));

TEST_F(Mem03Fx, NoLabelCounters) {
    m().RecordCrossUserBlocked();
    m().RecordCrossUserBlocked();
    m().RecordEditConflict();
    EXPECT_EQ(m().CrossUserBlockedCount(), 2u);
    EXPECT_EQ(m().EditConflictCount(), 1u);
}

// op_latency histogram per op, bounds {0.005,...,1}s.
TEST_F(Mem03Fx, OpLatencyHistogramRender) {
    m().ObserveOpLatency(Op::kList, 5);    // 0.005s exactly first bound
    m().ObserveOpLatency(Op::kList, 2);    // under
    m().ObserveOpLatency(Op::kList, 99999);  // above largest -> +Inf
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_memory_transparency_op_latency_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("op=\"list\""), std::string::npos);
}

TEST_F(Mem03Fx, OpLatencyNegativeClamps) {
    m().ObserveOpLatency(Op::kEdit, -3);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("op=\"edit\""), std::string::npos);
}

TEST_F(Mem03Fx, RenderHelpTypeNoHighCardinality) {
    m().RecordOp(Op::kList, OS::kSuccess);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_memory_transparency_op_total counter"),
              std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(Mem03Fx, Reset) {
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
