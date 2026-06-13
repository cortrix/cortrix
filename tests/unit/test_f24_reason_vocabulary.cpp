#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/deploy/reason_vocabulary.h"

// F24-S7/S8 coverage: the OpenMetrics `reason` controlled vocabulary (§8, F24-6),
// the reason<->error_code naming alignment (§9.3, F24-7), and the label vs
// structured_data field-ownership table (§9.2, F24-7).
namespace cortrix::deploy {
namespace {

using agent_friendly::ErrorCategory;

TEST(ReasonVocabularyTest, CountMatchesEnumeration) {
    EXPECT_EQ(kMetricReasonCount, 23);
    EXPECT_EQ(AllReasons().size(), static_cast<size_t>(kMetricReasonCount));
}

TEST(ReasonVocabularyTest, EveryReasonIsDottedSubsystemPrefixed) {
    std::set<std::string> seen;
    for (MetricReason r : AllReasons()) {
        std::string s = ReasonString(r);
        EXPECT_FALSE(s.empty());
        // pattern <subsystem>.<action_outcome>: exactly one dot, lowercase.
        auto dot = s.find('.');
        ASSERT_NE(dot, std::string::npos) << s << " must contain a dot";
        EXPECT_EQ(s.find('.', dot + 1), std::string::npos) << s << " must have one dot";
        for (char c : s) {
            EXPECT_TRUE(c == '.' || c == '_' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                << "unexpected char in reason " << s;
        }
        // subsystem prefix matches the text before the dot.
        EXPECT_EQ(s.substr(0, dot), ReasonSubsystem(r)) << s;
        EXPECT_TRUE(seen.insert(s).second) << "duplicate reason: " << s;
    }
    EXPECT_EQ(seen.size(), 23u);
}

TEST(ReasonVocabularyTest, IsValidReasonAcceptsMembersRejectsOthers) {
    EXPECT_TRUE(IsValidReason("llm.budget_exceeded"));
    EXPECT_TRUE(IsValidReason("catalog.bf_not_ready"));
    EXPECT_TRUE(IsValidReason("disk.threshold_crit"));
    EXPECT_FALSE(IsValidReason("llm.made_up_reason"));
    EXPECT_FALSE(IsValidReason("not_dotted"));
    EXPECT_FALSE(IsValidReason(""));
}

TEST(ReasonVocabularyTest, ReasonToErrorCodeFollowsAlignmentRule) {
    // §9.3 examples.
    EXPECT_EQ(ReasonToErrorCode("llm.budget_exceeded"), "CX_ERR_LLM_BUDGET_EXCEEDED");
    EXPECT_EQ(ReasonToErrorCode("spc.queue_full"), "CX_ERR_SPC_QUEUE_FULL");
    EXPECT_EQ(ReasonToErrorCode("catalog.bf_not_ready"), "CX_ERR_CATALOG_BF_NOT_READY");
    EXPECT_EQ(ReasonToErrorCode("ns_routing.unauthorized"), "CX_ERR_NS_ROUTING_UNAUTHORIZED");
}

TEST(ReasonVocabularyTest, ErrorCodeToReasonIsInverseOnFirstToken) {
    EXPECT_EQ(ErrorCodeToReason("CX_ERR_SPC_QUEUE_FULL"), "spc.queue_full");
    EXPECT_EQ(ErrorCodeToReason("CX_ERR_LLM_BUDGET_EXCEEDED"), "llm.budget_exceeded");
    // round-trip on a single-subsystem-token reason.
    std::string reason = "disk.threshold_crit";
    EXPECT_EQ(ErrorCodeToReason(ReasonToErrorCode(reason)), reason);
}

TEST(CategoryAlignmentTest, FiveCategoryStringsMatchAgentFriendly) {
    // F24-7: the metric category label uses the same serialization as the error body.
    EXPECT_STREQ(CategoryString(ErrorCategory::kAuth), "auth");
    EXPECT_STREQ(CategoryString(ErrorCategory::kQuota), "quota");
    EXPECT_STREQ(CategoryString(ErrorCategory::kTransient), "transient");
    EXPECT_STREQ(CategoryString(ErrorCategory::kPermanent), "permanent");
    EXPECT_STREQ(CategoryString(ErrorCategory::kTimeout), "timeout");
}

TEST(FieldOwnershipTest, HighCardinalityFieldsAreStructuredOnly) {
    // §9.2 — the OBS_SPEC §3.2 deny-list fields never become labels.
    EXPECT_EQ(ChannelFor(FieldKind::kHighCardId), FieldChannel::kStructuredOnly);
    EXPECT_EQ(ChannelFor(FieldKind::kNumericValue), FieldChannel::kStructuredOnly);
    EXPECT_EQ(ChannelFor(FieldKind::kBusinessObject), FieldChannel::kStructuredOnly);
}

TEST(FieldOwnershipTest, CategoryIsBothAndConsistent) {
    EXPECT_EQ(ChannelFor(FieldKind::kCategory), FieldChannel::kBoth);
    EXPECT_EQ(ChannelFor(FieldKind::kPlanRegionInstance), FieldChannel::kBoth);
}

TEST(FieldOwnershipTest, ReasonIsLabelOnly) {
    EXPECT_EQ(ChannelFor(FieldKind::kReason), FieldChannel::kLabelOnly);
}

}  // namespace
}  // namespace cortrix::deploy
