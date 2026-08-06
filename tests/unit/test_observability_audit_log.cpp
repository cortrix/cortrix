#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/agent_trace/observability_audit_log.h"

// S7 coverage (admin cross-user forensics): the structured JSON log line
// shape + endpoint labels + Emit() writing to stderr.
namespace cortrix::agent_trace {
namespace {

TEST(ObservabilityAuditLogTest, BuildLineShape) {
    std::string line = ObservabilityAuditLog::BuildLine(
        "admin-1", "alice", ObservabilityAuditLog::Endpoint::kTraces, 1764547200123LL);
    auto j = nlohmann::json::parse(line);
    EXPECT_EQ(j["event"], "admin_cross_user_access");
    EXPECT_EQ(j["level"], "warn");
    EXPECT_EQ(j["admin_user_id"], "admin-1");
    EXPECT_EQ(j["target_user_id"], "alice");
    EXPECT_EQ(j["endpoint"], "traces");
    EXPECT_EQ(j["timestamp_ms"], 1764547200123LL);
}

TEST(ObservabilityAuditLogTest, EndpointLabels) {
    EXPECT_STREQ(ObservabilityAuditLog::ToString(
                     ObservabilityAuditLog::Endpoint::kTraces), "traces");
    EXPECT_STREQ(ObservabilityAuditLog::ToString(
                     ObservabilityAuditLog::Endpoint::kInteractions), "interactions");
    EXPECT_STREQ(ObservabilityAuditLog::ToString(
                     ObservabilityAuditLog::Endpoint::kInteractionsSources),
                 "interactions_sources");
}

TEST(ObservabilityAuditLogTest, EmptyTargetIsAllowed) {
    // An anonymous owner (NULL user_id) yields an empty target_user_id — still valid.
    std::string line = ObservabilityAuditLog::BuildLine(
        "root", "", ObservabilityAuditLog::Endpoint::kInteractions, 1);
    auto j = nlohmann::json::parse(line);
    EXPECT_EQ(j["target_user_id"], "");
}

TEST(ObservabilityAuditLogTest, EmitWritesStructuredLineToStderr) {
    testing::internal::CaptureStderr();
    ObservabilityAuditLog::Emit("root", "alice",
                                ObservabilityAuditLog::Endpoint::kInteractionsSources);
    std::string err = testing::internal::GetCapturedStderr();
    auto j = nlohmann::json::parse(err);  // one line + trailing newline
    EXPECT_EQ(j["event"], "admin_cross_user_access");
    EXPECT_EQ(j["admin_user_id"], "root");
    EXPECT_EQ(j["target_user_id"], "alice");
    EXPECT_EQ(j["endpoint"], "interactions_sources");
    EXPECT_GT(j["timestamp_ms"].get<int64_t>(), 0);
}

}  // namespace
}  // namespace cortrix::agent_trace
