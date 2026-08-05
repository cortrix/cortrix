// JSON parse-tolerance matrix for ContradictionDetector::ParseJudgmentJson.
//
// Real parser (src/memory/contradiction_detector.cpp ParseJudgmentJson, static):
//   1. json::parse(allow_exceptions=false). If discarded OR not an object ->
//      error Result (Mem02Status kExtractInvalidOutput, message carries
//      "CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT").  *** NO fence / wrapper / span
//      recovery here *** (unlike ParseExtractionJson). A fenced or prose-wrapped
//      body is NOT a JSON object -> rejected.
//   2. "is_contradiction" REQUIRED: bool, or a number (!=0 -> true). Missing or a
//      non-bool/non-number type -> error.
//   3. "reason" string optional; "confidence" number optional, clamped [0,1].
//
// So the tolerance contract is STRICTER than the extractor: only a clean JSON
// object with a usable is_contradiction parses; everything wrapped/fenced/truncated
// is gracefully rejected (no crash).

#include <gtest/gtest.h>

#include <string>

#include "cortrix/common/result.h"
#include "cortrix/memory/contradiction_detector.h"
#include "cortrix/memory/memory_extractor.h"

namespace cortrix::memory {
namespace {

using Judgment = MemoryExtractor::Judgment;

enum class JTol {
    kParsed,    // object parses; expect_contra is the expected is_contradiction
    kRejected,  // graceful error Result
};

struct JudgmentJsonCase {
    const char* name;
    std::string input;
    JTol expect;
    bool expect_contra;  // only meaningful when expect == kParsed
};

class JudgmentJsonMatrix : public ::testing::TestWithParam<JudgmentJsonCase> {};

TEST_P(JudgmentJsonMatrix, Tolerance) {
    const JudgmentJsonCase& tc = GetParam();
    Result<Judgment> r = ContradictionDetector::ParseJudgmentJson(tc.input);
    if (tc.expect == JTol::kParsed) {
        ASSERT_TRUE(r.ok()) << tc.name << " status=" << r.status().message();
        EXPECT_EQ(r.value().is_contradiction, tc.expect_contra) << tc.name;
    } else {
        ASSERT_FALSE(r.ok()) << tc.name << " unexpectedly parsed";
        EXPECT_NE(r.status().message().find("CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT"),
                  std::string::npos)
            << tc.name << " msg=" << r.status().message();
    }
}

const std::vector<JudgmentJsonCase> kJudgmentCases = {
    // ---- clean valid ----
    {"clean_true", R"({"is_contradiction":true,"reason":"r","confidence":0.8})",
     JTol::kParsed, true},
    {"clean_false", R"({"is_contradiction":false})", JTol::kParsed, false},
    {"clean_num_one", R"({"is_contradiction":1})", JTol::kParsed, true},
    {"clean_num_zero", R"({"is_contradiction":0})", JTol::kParsed, false},
    {"clean_num_nonzero", R"({"is_contradiction":0.4})", JTol::kParsed, true},
    {"clean_conf_clamped_high",
     R"({"is_contradiction":true,"confidence":9.0})", JTol::kParsed, true},
    {"clean_conf_clamped_low",
     R"({"is_contradiction":true,"confidence":-3.0})", JTol::kParsed, true},
    {"clean_extra_keys",
     R"({"is_contradiction":true,"explanation":"x","unrelated":[1,2]})",
     JTol::kParsed, true},

    // ---- ```json fenced -> NOT an object, rejected ----
    {"fenced_json",
     "```json\n{\"is_contradiction\":true}\n```", JTol::kRejected, false},
    // ---- ``` bare-fenced -> rejected ----
    {"fenced_bare", "```\n{\"is_contradiction\":false}\n```", JTol::kRejected, false},

    // ---- object-wrapped (extra nesting; top-level object lacks is_contradiction) ----
    {"wrapped_result",
     R"({"result":{"is_contradiction":true}})", JTol::kRejected, false},
    {"wrapped_judgment",
     R"({"judgment":{"is_contradiction":false}})", JTol::kRejected, false},

    // ---- prose-surrounded -> rejected ----
    {"prose_before",
     "The verdict is: {\"is_contradiction\":true}", JTol::kRejected, false},
    {"prose_after",
     "{\"is_contradiction\":true} (high confidence)", JTol::kRejected, false},

    // ---- leading/trailing whitespace: nlohmann tolerates surrounding ws ----
    {"ws_leading", "   \n\t{\"is_contradiction\":true}", JTol::kParsed, true},
    {"ws_trailing", "{\"is_contradiction\":false}\n\n  ", JTol::kParsed, false},

    // ---- NaN / Infinity literals -> non-standard -> discarded -> reject ----
    {"nan_confidence",
     R"({"is_contradiction":true,"confidence":NaN})", JTol::kRejected, false},
    {"infinity_confidence",
     R"({"is_contradiction":true,"confidence":Infinity})", JTol::kRejected, false},

    // ---- duplicate keys: nlohmann keeps last ----
    {"duplicate_is_contradiction",
     R"({"is_contradiction":false,"is_contradiction":true})", JTol::kParsed, true},

    // ---- truncated ----
    {"truncated_object", R"({"is_contradiction":tr)", JTol::kRejected, false},
    {"truncated_no_close", R"({"is_contradiction":true)", JTol::kRejected, false},

    // ---- empty string ----
    {"empty_string", "", JTol::kRejected, false},
    {"whitespace_only", "   \n  ", JTol::kRejected, false},

    // ---- non-object-when-object-expected ----
    {"array_top_level", R"([{"is_contradiction":true}])", JTol::kRejected, false},
    {"scalar_number", "1", JTol::kRejected, false},
    {"scalar_string", R"("true")", JTol::kRejected, false},
    {"json_null", "null", JTol::kRejected, false},

    // ---- object but missing / wrong-typed is_contradiction ----
    {"missing_is_contradiction", R"({"reason":"no verdict"})", JTol::kRejected, false},
    {"is_contradiction_string",
     R"({"is_contradiction":"true"})", JTol::kRejected, false},
    {"is_contradiction_null",
     R"({"is_contradiction":null})", JTol::kRejected, false},
    {"is_contradiction_array",
     R"({"is_contradiction":[true]})", JTol::kRejected, false},

    // ---- BOM-prefixed: nlohmann::parse skips a leading UTF-8 BOM -> parses ----
    {"bom_prefixed",
     std::string("\xEF\xBB\xBF") + R"({"is_contradiction":true})", JTol::kParsed, true},

    // ---- nested-bracket inside reason string is fine ----
    {"nested_bracket_in_reason",
     R"({"is_contradiction":true,"reason":"see set {a,b} vs [c]"})",
     JTol::kParsed, true},
};

INSTANTIATE_TEST_SUITE_P(JudgmentJson, JudgmentJsonMatrix,
                         ::testing::ValuesIn(kJudgmentCases),
                         [](const ::testing::TestParamInfo<JudgmentJsonCase>& i) {
                             return std::string(i.param.name);
                         });

}  // namespace
}  // namespace cortrix::memory
