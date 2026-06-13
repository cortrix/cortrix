#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "cortrix/agent_friendly/error.h"

namespace cortrix::agent_friendly {
namespace {

TEST(AgentFriendlyErrorTest, CategorySerializesLowercase) {
    EXPECT_STREQ(ToString(ErrorCategory::kAuth), "auth");
    EXPECT_STREQ(ToString(ErrorCategory::kQuota), "quota");
    EXPECT_STREQ(ToString(ErrorCategory::kTransient), "transient");
    EXPECT_STREQ(ToString(ErrorCategory::kPermanent), "permanent");
    EXPECT_STREQ(ToString(ErrorCategory::kTimeout), "timeout");
}

TEST(AgentFriendlyErrorTest, ToJsonFullSchema) {
    AgentFriendlyError err;
    err.code = "CX_ERR_NS_UNAUTHORIZED";
    err.message = "no access to namespace ns_a";
    err.retryable = false;
    err.category = ErrorCategory::kAuth;
    err.structured_data = json{{"namespace", "ns_a"}};

    json j = ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_EQ(j["message"], "no access to namespace ns_a");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "auth");
    EXPECT_TRUE(j["retry_after_ms"].is_null());  // unset -> null
    EXPECT_EQ(j["structured_data"]["namespace"], "ns_a");
}

TEST(AgentFriendlyErrorTest, RetryAfterMsSerializesWhenSet) {
    AgentFriendlyError err;
    err.code = "CX_ERR_RATE_LIMITED";
    err.retryable = true;
    err.category = ErrorCategory::kQuota;
    err.retry_after_ms = 1000;

    json j = ToJson(err);
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "quota");
    EXPECT_EQ(j["retry_after_ms"], 1000);
    EXPECT_TRUE(j["structured_data"].is_null());  // unset -> null
}

TEST(AgentFriendlyExceptionTest, CarriesErrorAndWhat) {
    AgentFriendlyError err;
    err.code = "CX_ERR_TIMEOUT";
    err.message = "deadline exceeded";
    err.category = ErrorCategory::kTimeout;
    err.retryable = true;

    try {
        throw AgentFriendlyException(err);
    } catch (const AgentFriendlyException& e) {
        EXPECT_EQ(e.GetError().code, "CX_ERR_TIMEOUT");
        EXPECT_EQ(e.GetError().category, ErrorCategory::kTimeout);
        EXPECT_NE(std::string(e.what()).find("CX_ERR_TIMEOUT"), std::string::npos);
    }
}

}  // namespace
}  // namespace cortrix::agent_friendly
