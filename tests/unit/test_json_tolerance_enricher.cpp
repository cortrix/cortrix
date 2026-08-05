// JSON parse-tolerance matrix for cortrix::spc::ParseEnrichBatchResponse (SPC enricher,
// topic 3.3 three-layer fault tolerance).
//
// Real parser (src/spc/enricher_response_parser.cpp): NEVER throws and NEVER returns
// an error wrapper -- it ALWAYS returns a std::vector<EnrichResult> of exactly
// batch_size elements (index-aligned), encoding tolerance via per-element status:
//   - L3 (whole batch): body not a parseable JSON OBJECT (discarded or non-object)
//       -> EVERY element status = (int)EnricherErrorCode::kParse, score 0,
//       error_msg contains "CX_ERR_ENRICHER_PARSE (L3)".
//   - L2 (chunk missing): object parses but the index key (its stringified i, e.g.
//       "0") is absent or not an object -> that element status = kParse, layer L2.
//   - L1 (field missing/malformed inside a present chunk): defaults applied, element
//       stays status = 0 (success).  entities missing -> empty; summary missing ->
//       ""; score missing/non-number -> 0; score clamped to [0,1].
//
// The parser accepts a complete Markdown JSON fence (```json ... ```) because
// live OpenAI-compatible providers may return that despite response_format.
// It still does not recover JSON from surrounding prose or partial text. The
// invariant under test is "no crash, correct vector size, documented layer
// status" -- never a thrown exception.
//
// kClean=the 2-chunk happy body, expected status [0,0].  We parameterize the body +
// the expected per-index status vector (length 2 unless noted via batch_size).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_error.h"
#include "cortrix/spc_enricher/enricher_response_parser.h"

namespace cortrix::spc {
namespace {

const int kParseStatus = static_cast<int>(EnricherErrorCode::kParse);

struct EnricherJsonCase {
    const char* name;
    std::string body;
    int batch_size;
    std::vector<int> expected_status;  // size must == batch_size
};

class EnricherJsonMatrix : public ::testing::TestWithParam<EnricherJsonCase> {};

TEST_P(EnricherJsonMatrix, Tolerance) {
    const EnricherJsonCase& tc = GetParam();
    // No crash, no throw -- the parser is total.
    std::vector<EnrichResult> r =
        ParseEnrichBatchResponse(tc.body, tc.batch_size, "gpt-4o-mini", "default-zh");
    // Invariant 1: result is always exactly batch_size long, index-aligned.
    ASSERT_EQ(static_cast<int>(r.size()), tc.batch_size) << tc.name;
    ASSERT_EQ(tc.expected_status.size(), r.size()) << tc.name << " (test spec mismatch)";
    // Invariant 2: documented per-index layer status.
    for (size_t i = 0; i < r.size(); ++i) {
        EXPECT_EQ(r[i].status, tc.expected_status[i])
            << tc.name << " idx=" << i << " msg=" << r[i].error_msg;
        if (tc.expected_status[i] == kParseStatus) {
            // A parse-failed element scores 0 and carries the CX token.
            EXPECT_FLOAT_EQ(r[i].enriched_score, 0.0f) << tc.name << " idx=" << i;
            EXPECT_NE(r[i].error_msg.find("CX_ERR_ENRICHER_PARSE"), std::string::npos)
                << tc.name << " idx=" << i;
        }
    }
}

// 2-chunk happy body (both indices present, valid).
const char* kClean =
    R"({"0":{"entities":[{"text":"Cortrix","type":"PRODUCT","start_offset":0,"end_offset":7}],)"
    R"("summary":"about Cortrix","score":0.9},)"
    R"("1":{"entities":[],"summary":"second","score":0.5}})";

const std::vector<EnricherJsonCase> kEnricherCases = {
    // ---- clean valid (both chunks succeed L1) ----
    {"clean_two", std::string(kClean), 2, {0, 0}},
    {"clean_one", R"({"0":{"summary":"only","score":0.3}})", 1, {0}},

    // ---- L1 field-default tolerance: present chunk, missing fields -> still success ----
    {"l1_missing_entities", R"({"0":{"summary":"s","score":0.2}})", 1, {0}},
    {"l1_missing_summary", R"({"0":{"score":0.2}})", 1, {0}},
    {"l1_missing_score", R"({"0":{"summary":"s"}})", 1, {0}},
    {"l1_score_nonnumber", R"({"0":{"summary":"s","score":"high"}})", 1, {0}},
    {"l1_score_clamped_high", R"({"0":{"summary":"s","score":5.0}})", 1, {0}},
    {"l1_score_clamped_low", R"({"0":{"summary":"s","score":-5.0}})", 1, {0}},
    {"l1_empty_object_chunk", R"({"0":{}})", 1, {0}},
    {"l1_malformed_entity_skipped",
     R"({"0":{"entities":[1,"bad",{"text":"ok"}],"summary":"s"}})", 1, {0}},
    {"l1_entities_not_array", R"({"0":{"entities":"oops","summary":"s"}})", 1, {0}},

    // ---- L2 chunk-missing: object parses, index key absent ----
    {"l2_missing_index1", R"({"0":{"summary":"s"}})", 2, {0, kParseStatus}},
    {"l2_all_missing", R"({"99":{"summary":"s"}})", 2, {kParseStatus, kParseStatus}},
    {"l2_index_not_object", R"({"0":"not an object","1":{"summary":"s"}})",
     2, {kParseStatus, 0}},
    {"l2_empty_object_body", "{}", 2, {kParseStatus, kParseStatus}},

    // ---- complete fenced JSON -> unwraps to the clean object ----
    {"l3_fenced_json", "```json\n" + std::string(kClean) + "\n```",
     2, {0, 0}},
    // ---- complete bare-fenced JSON -> unwraps to the clean object ----
    {"l3_fenced_bare", "```\n" + std::string(kClean) + "\n```",
     2, {0, 0}},

    // ---- object-wrapped: parses to object but index keys nested -> L2 all ----
    {"l2_wrapped_result", R"({"result":)" + std::string(kClean) + "}",
     2, {kParseStatus, kParseStatus}},
    {"l2_wrapped_chunks", R"({"chunks":)" + std::string(kClean) + "}",
     2, {kParseStatus, kParseStatus}},

    // ---- prose-surrounded -> not parseable object -> L3 all ----
    {"l3_prose_before", "Here you go:\n" + std::string(kClean),
     2, {kParseStatus, kParseStatus}},
    {"l3_prose_after", std::string(kClean) + "\nDone.", 2, {kParseStatus, kParseStatus}},

    // ---- leading/trailing whitespace: nlohmann tolerates -> normal parse ----
    {"ws_leading", "  \n\t" + std::string(kClean), 2, {0, 0}},
    {"ws_trailing", std::string(kClean) + "\n\n  ", 2, {0, 0}},

    // ---- NaN / Infinity literals -> non-standard -> discarded -> L3 all ----
    {"l3_nan_literal", R"({"0":{"score":NaN,"summary":"s"}})", 1, {kParseStatus}},
    {"l3_infinity_literal", R"({"0":{"score":Infinity,"summary":"s"}})",
     1, {kParseStatus}},

    // ---- duplicate index keys: nlohmann keeps last; still object -> L1 success ----
    {"duplicate_index_key",
     R"({"0":{"summary":"first"},"0":{"summary":"second","score":0.4}})", 1, {0}},

    // ---- truncated -> discarded -> L3 all ----
    {"l3_truncated", R"({"0":{"summary":"s")", 2, {kParseStatus, kParseStatus}},

    // ---- empty string -> discarded -> L3 all ----
    {"l3_empty_string", "", 2, {kParseStatus, kParseStatus}},
    {"l3_whitespace_only", "   \n  ", 2, {kParseStatus, kParseStatus}},

    // ---- non-object top-level (array / scalar) -> L3 all ----
    {"l3_array_top_level", "[" + std::string(kClean) + "]",
     2, {kParseStatus, kParseStatus}},
    {"l3_scalar_number", "42", 1, {kParseStatus}},
    {"l3_scalar_string", R"("hi")", 1, {kParseStatus}},
    {"l3_json_null", "null", 1, {kParseStatus}},

    // ---- BOM-prefixed: nlohmann skips BOM -> normal object parse ----
    {"bom_prefixed", std::string("\xEF\xBB\xBF") + std::string(kClean), 2, {0, 0}},

    // ---- nested-bracket inside a string field is fine (L1 success) ----
    {"nested_bracket_in_summary",
     R"({"0":{"summary":"covers [1,2] and {a:b}","score":0.1}})", 1, {0}},

    // ---- numeric string entity offsets are field-level recoverable ----
    {"l1_numeric_string_offsets",
     R"({"0":{"entities":[{"text":"Cortrix","type":"PRODUCT","start_offset":"0","end_offset":"7"}],"summary":"s"}})",
     1, {0}},

    // ---- batch_size 0 -> empty vector, no crash ----
    {"batch_size_zero", std::string(kClean), 0, {}},

    // ---- batch_size larger than provided indices -> trailing L2 ----
    {"batch_larger_than_body", R"({"0":{"summary":"s"}})", 3,
     {0, kParseStatus, kParseStatus}},
};

INSTANTIATE_TEST_SUITE_P(EnricherJson, EnricherJsonMatrix,
                         ::testing::ValuesIn(kEnricherCases),
                         [](const ::testing::TestParamInfo<EnricherJsonCase>& i) {
                             return std::string(i.param.name);
                         });

}  // namespace
}  // namespace cortrix::spc
