#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_trace/observability_audit_log.h"
#include "cortrix/deploy/disk_monitor.h"
#include "cortrix/doc_summary/doc_summary_types.h"
#include "cortrix/observability/observability_context.h"
#include "cortrix/security/i_secret_provider.h"
#include "cortrix/spc_enricher/enricher_startup.h"
#include "cortrix/middleware/rate_limiter.h"

// Exhaustive ToString matrices for the infra / observability / deploy enums.
// These are all one-way (ToString only, no Parse surface): the contract pinned
// is exact token + lowercase + global uniqueness over every enum value. Each is
// a serialization label (status/stage/level/error/check) consumed by Agents.
//   DocSummaryStatus (cortrix::doc_summary)   3 values
//   RateLimitStratum (cortrix::middleware)    3 values
//   DiskStage        (cortrix::deploy)        3 values
//   LogLevel         (cortrix::observability) 4 values
//   SecretError      (cortrix::security)      5 values
//   StartupCheck     (cortrix::spc)           4 values
//   Endpoint         (cortrix::agent_trace::ObservabilityAuditLog) 3 values
// Suite names unique with RtInfra* / per-enum prefixes.

namespace cortrix::doc_summary {
namespace {

const std::vector<DocSummaryStatus>& AllDocSummaryStatuses() {
    static const std::vector<DocSummaryStatus> v = {
        DocSummaryStatus::kPending, DocSummaryStatus::kGenerated,
        DocSummaryStatus::kFailed};
    return v;
}

class RtDocSummaryStatusMatrix
    : public ::testing::TestWithParam<DocSummaryStatus> {};

TEST_P(RtDocSummaryStatusMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_');
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtDocSummaryStatusMatrix,
                         ::testing::ValuesIn(AllDocSummaryStatuses()));

TEST(RtDocSummaryStatusTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(DocSummaryStatus::kPending), "pending");
    EXPECT_STREQ(ToString(DocSummaryStatus::kGenerated), "generated");
    EXPECT_STREQ(ToString(DocSummaryStatus::kFailed), "failed");
    std::set<std::string> seen;
    for (DocSummaryStatus s : AllDocSummaryStatuses())
        EXPECT_TRUE(seen.insert(ToString(s)).second);
    EXPECT_EQ(seen.size(), 3u);
}

}  // namespace
}  // namespace cortrix::doc_summary

namespace cortrix::middleware {
namespace {

const std::vector<RateLimitStratum>& AllStrata() {
    static const std::vector<RateLimitStratum> v = {
        RateLimitStratum::kPerIp, RateLimitStratum::kPerApiKey,
        RateLimitStratum::kGlobal};
    return v;
}

class RtRateLimitStratumMatrix
    : public ::testing::TestWithParam<RateLimitStratum> {};

TEST_P(RtRateLimitStratumMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_');
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtRateLimitStratumMatrix,
                         ::testing::ValuesIn(AllStrata()));

TEST(RtRateLimitStratumTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(RateLimitStratum::kPerIp), "per_ip");
    EXPECT_STREQ(ToString(RateLimitStratum::kPerApiKey), "per_api_key");
    EXPECT_STREQ(ToString(RateLimitStratum::kGlobal), "global");
    std::set<std::string> seen;
    for (RateLimitStratum s : AllStrata())
        EXPECT_TRUE(seen.insert(ToString(s)).second);
    EXPECT_EQ(seen.size(), 3u);
}

}  // namespace
}  // namespace cortrix::middleware

namespace cortrix::deploy {
namespace {

const std::vector<DiskStage>& AllDiskStages() {
    static const std::vector<DiskStage> v = {
        DiskStage::kNormal, DiskStage::kWarn, DiskStage::kCrit};
    return v;
}

class RtDiskStageMatrix : public ::testing::TestWithParam<DiskStage> {};

TEST_P(RtDiskStageMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_');
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtDiskStageMatrix,
                         ::testing::ValuesIn(AllDiskStages()));

TEST(RtDiskStageTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(DiskStage::kNormal), "normal");
    EXPECT_STREQ(ToString(DiskStage::kWarn), "warn");
    EXPECT_STREQ(ToString(DiskStage::kCrit), "crit");
    std::set<std::string> seen;
    for (DiskStage s : AllDiskStages())
        EXPECT_TRUE(seen.insert(ToString(s)).second);
    EXPECT_EQ(seen.size(), 3u);
}

}  // namespace
}  // namespace cortrix::deploy

namespace cortrix::observability {
namespace {

const std::vector<LogLevel>& AllLogLevels() {
    static const std::vector<LogLevel> v = {LogLevel::kDebug, LogLevel::kInfo,
                                            LogLevel::kWarn, LogLevel::kError};
    return v;
}

class RtLogLevelMatrix : public ::testing::TestWithParam<LogLevel> {};

TEST_P(RtLogLevelMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE(c >= 'a' && c <= 'z');
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtLogLevelMatrix,
                         ::testing::ValuesIn(AllLogLevels()));

TEST(RtLogLevelTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(LogLevel::kDebug), "debug");
    EXPECT_STREQ(ToString(LogLevel::kInfo), "info");
    EXPECT_STREQ(ToString(LogLevel::kWarn), "warn");
    EXPECT_STREQ(ToString(LogLevel::kError), "error");
    std::set<std::string> seen;
    for (LogLevel l : AllLogLevels())
        EXPECT_TRUE(seen.insert(ToString(l)).second);
    EXPECT_EQ(seen.size(), 4u);
}

}  // namespace
}  // namespace cortrix::observability

namespace cortrix::security {
namespace {

const std::vector<SecretError>& AllSecretErrors() {
    static const std::vector<SecretError> v = {
        SecretError::kNotFound, SecretError::kProviderUnavailable,
        SecretError::kPermissionDenied, SecretError::kNetworkTimeout,
        SecretError::kInvalidConfig};
    return v;
}

class RtSecretErrorMatrix : public ::testing::TestWithParam<SecretError> {};

TEST_P(RtSecretErrorMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_');
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtSecretErrorMatrix,
                         ::testing::ValuesIn(AllSecretErrors()));

TEST(RtSecretErrorTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(SecretError::kNotFound), "not_found");
    EXPECT_STREQ(ToString(SecretError::kProviderUnavailable),
                 "provider_unavailable");
    EXPECT_STREQ(ToString(SecretError::kPermissionDenied), "permission_denied");
    EXPECT_STREQ(ToString(SecretError::kNetworkTimeout), "network_timeout");
    EXPECT_STREQ(ToString(SecretError::kInvalidConfig), "invalid_config");
    std::set<std::string> seen;
    for (SecretError e : AllSecretErrors())
        EXPECT_TRUE(seen.insert(ToString(e)).second);
    EXPECT_EQ(seen.size(), 5u);
}

}  // namespace
}  // namespace cortrix::security

namespace cortrix::spc {
namespace {

const std::vector<StartupCheck>& AllStartupChecks() {
    static const std::vector<StartupCheck> v = {
        StartupCheck::kOk, StartupCheck::kNullType,
        StartupCheck::kApiKeyMissing, StartupCheck::kEndpointUnreachable};
    return v;
}

class RtStartupCheckMatrix : public ::testing::TestWithParam<StartupCheck> {};

TEST_P(RtStartupCheckMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_');
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtStartupCheckMatrix,
                         ::testing::ValuesIn(AllStartupChecks()));

TEST(RtStartupCheckTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(StartupCheck::kOk), "ok");
    EXPECT_STREQ(ToString(StartupCheck::kNullType), "null_type");
    EXPECT_STREQ(ToString(StartupCheck::kApiKeyMissing), "api_key_missing");
    EXPECT_STREQ(ToString(StartupCheck::kEndpointUnreachable),
                 "endpoint_unreachable");
    std::set<std::string> seen;
    for (StartupCheck c : AllStartupChecks())
        EXPECT_TRUE(seen.insert(ToString(c)).second);
    EXPECT_EQ(seen.size(), 4u);
}

}  // namespace
}  // namespace cortrix::spc

namespace cortrix::agent_trace {
namespace {

using Endpoint = ObservabilityAuditLog::Endpoint;

const std::vector<Endpoint>& AllAuditEndpoints() {
    static const std::vector<Endpoint> v = {Endpoint::kTraces,
                                            Endpoint::kInteractions,
                                            Endpoint::kInteractionsSources};
    return v;
}

class RtAuditEndpointMatrix : public ::testing::TestWithParam<Endpoint> {};

TEST_P(RtAuditEndpointMatrix, TokenNonEmptyLowercase) {
    const std::string token = ObservabilityAuditLog::ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_');
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtAuditEndpointMatrix,
                         ::testing::ValuesIn(AllAuditEndpoints()));

TEST(RtAuditEndpointTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ObservabilityAuditLog::ToString(Endpoint::kTraces), "traces");
    EXPECT_STREQ(ObservabilityAuditLog::ToString(Endpoint::kInteractions),
                 "interactions");
    EXPECT_STREQ(
        ObservabilityAuditLog::ToString(Endpoint::kInteractionsSources),
        "interactions_sources");
    std::set<std::string> seen;
    for (Endpoint e : AllAuditEndpoints())
        EXPECT_TRUE(seen.insert(ObservabilityAuditLog::ToString(e)).second);
    EXPECT_EQ(seen.size(), 3u);
}

}  // namespace
}  // namespace cortrix::agent_trace
