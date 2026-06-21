#include <gtest/gtest.h>

#include <string>

#include "cortrix/memory/contradiction_detector.h"
#include "cortrix/memory/mem02_error.h"

// DEPTH — a dedicated suite for ContradictionDetector::ParseJudgmentJson (the
// static single-object JSON parse contract used by the MEM02 D5 judge). The
// existing test_memory_extractor.cpp only smoke-tests valid / "not json" /
// missing-verdict via the MemoryExtractor delegate; this suite pins every
// reachable arm of the parser directly: the bool/numeric verdict accept, the
// confidence clamp (both directions + boundaries), reason optionality/typing,
// the array-instead-of-object reject, and the (DIVERGENCE-from-hypothesis)
// behaviour for code-fenced + {"result":{...}}-wrapped inputs — which this
// parser does NOT unwrap (no fence-strip / no result-unwrap in the impl), so
// they are rejected. Suite name is globally unique (ContradictionParseDepth).
namespace cortrix::memory {
namespace {

using Judgment = MemoryExtractor::Judgment;

// CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT is the only error identity this static
// parser returns; helper to assert it without restating the token everywhere.
bool IsInvalidOutput(const Status& st) {
    return !st.ok() &&
           st.message().find("CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT") != std::string::npos;
}

// ---------- happy single-object ----------

TEST(ContradictionParseDepthTest, BoolTrueVerdictWithReasonAndConfidence) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"reason":"location conflict","confidence":0.75})");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_TRUE(r.value().is_contradiction);
    EXPECT_EQ(r.value().reason, "location conflict");
    EXPECT_DOUBLE_EQ(r.value().confidence, 0.75);
}

TEST(ContradictionParseDepthTest, BoolFalseVerdict) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":false,"reason":"independent","confidence":0.9})");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().is_contradiction);
}

// ---------- numeric bool (0/1 + non-zero) ----------

TEST(ContradictionParseDepthTest, NumericOneIsTrueVerdict) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":1,"confidence":0.5})");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().is_contradiction);
}

TEST(ContradictionParseDepthTest, NumericZeroIsFalseVerdict) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":0,"confidence":0.5})");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().is_contradiction);
}

TEST(ContradictionParseDepthTest, NumericNonZeroFloatIsTrueVerdict) {
    // 0.4 != 0.0 → the `v.get<double>() != 0.0` arm yields true.
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":0.4,"confidence":0.6})");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().is_contradiction);
}

TEST(ContradictionParseDepthTest, StringVerdictIsRejected) {
    // A non-bool, non-number verdict is the explicit reject arm.
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":"true","confidence":0.6})");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(IsInvalidOutput(r.status())) << r.status().message();
}

// ---------- missing / malformed verdict ----------

TEST(ContradictionParseDepthTest, MissingVerdictKeyIsRejected) {
    auto r = ContradictionDetector::ParseJudgmentJson(R"({"reason":"no verdict"})");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(IsInvalidOutput(r.status()));
    EXPECT_NE(r.status().message().find("missing is_contradiction"), std::string::npos);
}

TEST(ContradictionParseDepthTest, NotJsonIsRejected) {
    auto r = ContradictionDetector::ParseJudgmentJson("this is not json");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(IsInvalidOutput(r.status()));
}

TEST(ContradictionParseDepthTest, EmptyStringIsRejected) {
    auto r = ContradictionDetector::ParseJudgmentJson("");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(IsInvalidOutput(r.status()));
}

// ---------- array instead of object ----------

TEST(ContradictionParseDepthTest, ArrayInsteadOfObjectIsRejected) {
    // Parses as valid JSON but !is_object() → the "not a JSON object" reject.
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"([{"is_contradiction":true,"confidence":0.9}])");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(IsInvalidOutput(r.status()));
    EXPECT_NE(r.status().message().find("not a JSON object"), std::string::npos);
}

TEST(ContradictionParseDepthTest, JsonScalarIsRejected) {
    auto r = ContradictionDetector::ParseJudgmentJson("true");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(IsInvalidOutput(r.status()));
}

TEST(ContradictionParseDepthTest, JsonNullIsRejected) {
    auto r = ContradictionDetector::ParseJudgmentJson("null");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(IsInvalidOutput(r.status()));
}

// ---------- reason optionality / typing ----------

TEST(ContradictionParseDepthTest, MissingReasonDefaultsEmpty) {
    // reason is optional; absent → empty string (verdict still parses).
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"confidence":0.8})");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().reason.empty());
    EXPECT_TRUE(r.value().is_contradiction);
}

TEST(ContradictionParseDepthTest, NonStringReasonIsIgnoredNotFatal) {
    // reason present but not a string → the `&& is_string()` guard skips it; the
    // judgment still parses with an empty reason (no reject).
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"reason":42,"confidence":0.8})");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().reason.empty());
}

// ---------- confidence handling ----------

TEST(ContradictionParseDepthTest, MissingConfidenceDefaultsZero) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"reason":"r"})");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.value().confidence, 0.0);
}

TEST(ContradictionParseDepthTest, NonNumericConfidenceIgnoredDefaultsZero) {
    // confidence present but a string → `&& is_number()` skips it → stays 0.0.
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"confidence":"high"})");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.value().confidence, 0.0);
}

TEST(ContradictionParseDepthTest, ConfidenceAboveOneClampsDown) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"confidence":1.7})");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.value().confidence, 1.0);
}

TEST(ContradictionParseDepthTest, ConfidenceBelowZeroClampsUp) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"confidence":-0.4})");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.value().confidence, 0.0);
}

TEST(ContradictionParseDepthTest, ConfidenceExactlyZeroBoundaryUnclamped) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":false,"confidence":0.0})");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.value().confidence, 0.0);
}

TEST(ContradictionParseDepthTest, ConfidenceExactlyOneBoundaryUnclamped) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"confidence":1.0})");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.value().confidence, 1.0);
}

TEST(ContradictionParseDepthTest, IntegerConfidenceCoercedToDouble) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":true,"confidence":1})");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.value().confidence, 1.0);
}

// ---------- DIVERGENCE notes: the impl does NOT unwrap fences / {"result":...} ----------
// The DEPTH brief hypothesised the parser handles a ```json fenced single-object
// and an object-wrapped {"result":{...}}. The frozen impl does NEITHER — it feeds
// the raw string straight to nlohmann::parse. These tests pin the ACTUAL contract
// (both rejected) so a future fence/unwrap addition is a deliberate, test-visible
// change rather than silent drift.

TEST(ContradictionParseDepthTest, CodeFencedObjectIsRejected_NoFenceStrip) {
    const std::string fenced =
        "```json\n{\"is_contradiction\":true,\"confidence\":0.9}\n```";
    auto r = ContradictionDetector::ParseJudgmentJson(fenced);
    ASSERT_FALSE(r.ok());  // leading ``` makes the whole string invalid JSON
    EXPECT_TRUE(IsInvalidOutput(r.status()));
}

TEST(ContradictionParseDepthTest, ResultWrappedObjectIsRejected_NoUnwrap) {
    // Valid JSON object, but the verdict lives under "result"; the parser looks
    // only at the top level → "missing is_contradiction".
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"result":{"is_contradiction":true,"confidence":0.9}})");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(IsInvalidOutput(r.status()));
    EXPECT_NE(r.status().message().find("missing is_contradiction"), std::string::npos);
}

// Whitespace around a bare object IS tolerated by nlohmann (the success contract
// the divergence cases contrast with).
TEST(ContradictionParseDepthTest, LeadingTrailingWhitespaceObjectAccepted) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        "  \n {\"is_contradiction\":true,\"confidence\":0.5} \n ");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().is_contradiction);
}

}  // namespace
}  // namespace cortrix::memory
