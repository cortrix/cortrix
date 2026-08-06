#include <gtest/gtest.h>

#include <string>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/observability/observability_context.h"

// Exhaustive char-class + length-boundary sweep over the agent trace identity validator
// (ObservabilityValidator::IsValidFormat + the three Validate* wrappers that
// share it). Whitelist (header) = [a-zA-Z0-9_.:/-], non-empty, length
// <= kMaxIdentityLength (128). A basic test exists elsewhere; this is the
// EXHAUSTIVE per-character + boundary matrix in NEW unique suites
// (ObsIdentityFormatMatrix / ObsIdentityWrapperMatrix).

namespace cortrix::observability {
namespace {

// ---- IsValidFormat: every accepted char + a swathe of rejected ones ----------

struct ObsIdentityFormatCase {
    std::string name;
    std::string value;
    bool expect_valid;
};

class ObsIdentityFormatMatrix
    : public ::testing::TestWithParam<ObsIdentityFormatCase> {};

TEST_P(ObsIdentityFormatMatrix, Format) {
    const auto& c = GetParam();
    EXPECT_EQ(
        ObservabilityValidator::IsValidFormat(
            c.value, ObservabilityValidator::kMaxIdentityLength),
        c.expect_valid)
        << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Obs, ObsIdentityFormatMatrix,
    ::testing::Values(
        // accepted classes
        ObsIdentityFormatCase{"lower", "abcxyz", true},
        ObsIdentityFormatCase{"upper", "ABCXYZ", true},
        ObsIdentityFormatCase{"digits", "0123456789", true},
        ObsIdentityFormatCase{"underscore", "a_b", true},
        ObsIdentityFormatCase{"dot", "a.b", true},
        ObsIdentityFormatCase{"colon", "a:b", true},
        ObsIdentityFormatCase{"slash", "a/b", true},
        ObsIdentityFormatCase{"hyphen", "a-b", true},
        ObsIdentityFormatCase{"all_allowed_punct", "_.:/-", true},
        ObsIdentityFormatCase{"trace_like", "trace/abc-123:def.0", true},
        ObsIdentityFormatCase{"single_char", "x", true},
        // length boundaries
        ObsIdentityFormatCase{"len_128_ok", std::string(128, 'a'), true},
        ObsIdentityFormatCase{"len_129_bad", std::string(129, 'a'), false},
        ObsIdentityFormatCase{"empty_bad", "", false},
        // rejected chars
        ObsIdentityFormatCase{"space_bad", "a b", false},
        ObsIdentityFormatCase{"tab_bad", std::string("a\tb"), false},
        ObsIdentityFormatCase{"newline_bad", std::string("a\nb"), false},
        ObsIdentityFormatCase{"semicolon_bad", "a;b", false},
        ObsIdentityFormatCase{"comma_bad", "a,b", false},
        ObsIdentityFormatCase{"at_bad", "a@b", false},
        ObsIdentityFormatCase{"hash_bad", "a#b", false},
        ObsIdentityFormatCase{"plus_bad", "a+b", false},
        ObsIdentityFormatCase{"equals_bad", "a=b", false},
        ObsIdentityFormatCase{"percent_bad", "a%b", false},
        ObsIdentityFormatCase{"quote_bad", std::string("a\"b"), false},
        ObsIdentityFormatCase{"backslash_bad", std::string("a\\b"), false},
        ObsIdentityFormatCase{"null_byte_bad", std::string("a\0b", 3), false},
        ObsIdentityFormatCase{"high_byte_bad", std::string("a\x80""b"), false},
        ObsIdentityFormatCase{"utf8_emoji_bad", std::string("a\xF0\x9F\x98\x80"), false}),
    [](const ::testing::TestParamInfo<ObsIdentityFormatCase>& i) { return i.param.name; });

// ---- Validate{Session,Trace,Agent}Id wrappers: value pass-through / error ----

enum class ObsField { kSession, kTrace, kAgent };

struct ObsWrapperCase {
    std::string name;
    ObsField field;
    std::string value;
    bool expect_ok;
};

class ObsIdentityWrapperMatrix
    : public ::testing::TestWithParam<ObsWrapperCase> {};

TEST_P(ObsIdentityWrapperMatrix, Wrapper) {
    const auto& c = GetParam();
    Result<std::string> r =
        c.field == ObsField::kSession
            ? ObservabilityValidator::ValidateSessionId(c.value)
            : c.field == ObsField::kTrace
                  ? ObservabilityValidator::ValidateTraceId(c.value)
                  : ObservabilityValidator::ValidateAgentId(c.value);
    EXPECT_EQ(r.ok(), c.expect_ok) << c.name;
    if (c.expect_ok) {
        EXPECT_EQ(r.value(), c.value) << c.name;  // value passes through unchanged
    } else {
        EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument) << c.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Obs, ObsIdentityWrapperMatrix,
    ::testing::Values(
        ObsWrapperCase{"session_ok", ObsField::kSession, "sess-001", true},
        ObsWrapperCase{"session_empty_bad", ObsField::kSession, "", false},
        ObsWrapperCase{"session_space_bad", ObsField::kSession, "bad id", false},
        ObsWrapperCase{"trace_ok", ObsField::kTrace, "trace.abc:123", true},
        ObsWrapperCase{"trace_over_len_bad", ObsField::kTrace, std::string(129, 'a'), false},
        ObsWrapperCase{"trace_semicolon_bad", ObsField::kTrace, "t;x", false},
        ObsWrapperCase{"agent_ok", ObsField::kAgent, "agent_42", true},
        ObsWrapperCase{"agent_empty_bad", ObsField::kAgent, "", false},
        ObsWrapperCase{"agent_at_bad", ObsField::kAgent, "a@b", false}),
    [](const ::testing::TestParamInfo<ObsWrapperCase>& i) { return i.param.name; });

}  // namespace
}  // namespace cortrix::observability
