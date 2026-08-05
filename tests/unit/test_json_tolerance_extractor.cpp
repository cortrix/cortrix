// JSON parse-tolerance matrix for MemoryExtractor::ParseExtractionJson.
//
// Real parser (src/memory/memory_extractor.cpp ParseExtractionJson):
//   1. json::parse(allow_exceptions=false). If discarded OR not an array, narrow
//      to the outermost [ ... ] span (find('[') .. rfind(']')) and re-parse.
//   2. If still discarded / not array  -> Result error (Mem02Status,
//      kExtractInvalidOutput, message carries "CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT").
//   3. Each element must be an object with a string "content", else error.
//      type missing/unknown -> kEvent (D4 fallback). confidence clamped [0,1].
//
// Documented tolerance therefore PARSES: clean array, ```json fenced array, bare
// ``` fenced array, object-wrapped {"memories":[...]}, prose-surrounded array,
// leading/trailing whitespace, nested-bracket content (outermost span still spans
// the whole array). It GRACEFULLY REJECTS (no crash, error Result): empty string,
// a non-array JSON object/scalar with no '[' .. ']' span, an array whose element
// lacks a string content. NaN/Infinity literals are non-standard JSON: nlohmann
// discards them, but the [ .. ] span re-parse is also discarded -> reject (asserted
// as "rejected", the documented graceful path, not a crash).
//
// ParseExtractionJson is a non-static member but touches no collaborator, so the
// fixture builds a MemoryExtractor with all-null seams (mirrors how the production
// header documents it as a pure, deterministic, unit-testable helper).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/memory/memory_extractor.h"

namespace cortrix::memory {
namespace {

enum class Tol {
    kParsed,    // parses to an array; expected_count is the element count
    kRejected,  // graceful error Result (CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT)
};

struct ExtractorJsonCase {
    const char* name;
    std::string input;
    Tol expect;
    int expected_count;  // only meaningful when expect == kParsed
};

// Build a fresh extractor with null seams (ParseExtractionJson uses none of them).
MemoryExtractor MakeExtractorJsonParser() {
    return MemoryExtractor(/*llm=*/nullptr, /*block_store=*/nullptr,
                           /*contradiction_query=*/nullptr, /*op_logger=*/nullptr,
                           MemoryExtractorConfig{});
}

class ExtractorJsonMatrix : public ::testing::TestWithParam<ExtractorJsonCase> {};

TEST_P(ExtractorJsonMatrix, Tolerance) {
    const ExtractorJsonCase& tc = GetParam();
    MemoryExtractor ex = MakeExtractorJsonParser();
    // No crash regardless of input.
    Result<std::vector<ExtractedMemory>> r = ex.ParseExtractionJson(tc.input);
    if (tc.expect == Tol::kParsed) {
        ASSERT_TRUE(r.ok()) << tc.name << " status=" << r.status().message();
        EXPECT_EQ(static_cast<int>(r.value().size()), tc.expected_count) << tc.name;
    } else {
        ASSERT_FALSE(r.ok()) << tc.name << " unexpectedly parsed";
        EXPECT_NE(r.status().message().find("CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT"),
                  std::string::npos)
            << tc.name << " msg=" << r.status().message();
    }
}

// A valid two-element array, reused inside wrappers.
const char* kArr2 =
    R"([{"content":"likes tea","type":"preference","confidence":0.9},)"
    R"({"content":"met Bob","type":"event"}])";

const std::vector<ExtractorJsonCase> kExtractorCases = {
    // ---- clean valid ----
    {"clean_empty_array", "[]", Tol::kParsed, 0},
    {"clean_one", R"([{"content":"a fact","type":"fact"}])", Tol::kParsed, 1},
    {"clean_two", std::string(kArr2), Tol::kParsed, 2},
    {"clean_content_only", R"([{"content":"x"}])", Tol::kParsed, 1},

    // ---- ```json fenced ----
    {"fenced_json", "```json\n" + std::string(kArr2) + "\n```", Tol::kParsed, 2},
    {"fenced_json_empty", "```json\n[]\n```", Tol::kParsed, 0},
    // ---- ``` bare-fenced ----
    {"fenced_bare", "```\n" + std::string(kArr2) + "\n```", Tol::kParsed, 2},

    // ---- object-wrapped ----
    {"wrapped_memories", R"({"memories":)" + std::string(kArr2) + "}", Tol::kParsed, 2},
    {"wrapped_result", R"({"result":)" + std::string(kArr2) + "}", Tol::kParsed, 2},
    {"wrapped_data_empty", R"({"data":[]})", Tol::kParsed, 0},

    // ---- prose-surrounded ----
    {"prose_before", "Here is the JSON:\n" + std::string(kArr2), Tol::kParsed, 2},
    {"prose_after", std::string(kArr2) + "\nThat is all.", Tol::kParsed, 2},
    {"prose_both",
     "Sure! " + std::string(kArr2) + " Hope this helps.", Tol::kParsed, 2},

    // ---- leading/trailing whitespace ----
    {"ws_leading", "   \n\t" + std::string(kArr2), Tol::kParsed, 2},
    {"ws_trailing", std::string(kArr2) + "\n\n   ", Tol::kParsed, 2},

    // ---- nested-bracket content (span still wraps whole array) ----
    {"nested_bracket_in_string",
     R"([{"content":"array literal [1,2,3] in text","type":"fact"}])",
     Tol::kParsed, 1},

    // ---- NaN / Infinity literals (non-standard) -> graceful reject ----
    {"nan_literal", R"([{"content":"x","confidence":NaN}])", Tol::kRejected, 0},
    {"infinity_literal", R"([{"content":"x","confidence":Infinity}])", Tol::kRejected, 0},

    // ---- duplicate keys: nlohmann keeps last; still a valid array element ----
    {"duplicate_keys",
     R"([{"content":"first","content":"second","type":"fact"}])", Tol::kParsed, 1},

    // ---- truncated ----
    {"truncated_array", R"([{"content":"x")", Tol::kRejected, 0},
    {"truncated_open_brace", R"([{"content")", Tol::kRejected, 0},

    // ---- empty string ----
    {"empty_string", "", Tol::kRejected, 0},
    {"whitespace_only", "   \n\t  ", Tol::kRejected, 0},

    // ---- non-array-when-array-expected (no [ .. ] span) ----
    {"object_no_array", R"({"content":"x"})", Tol::kRejected, 0},
    {"scalar_number", "42", Tol::kRejected, 0},
    {"scalar_string", R"("hello")", Tol::kRejected, 0},
    {"json_true", "true", Tol::kRejected, 0},
    {"json_null", "null", Tol::kRejected, 0},

    // ---- non-object array elements -> contract violation reject ----
    {"array_of_scalars", "[1,2,3]", Tol::kRejected, 0},
    {"array_element_missing_content", R"([{"type":"fact"}])", Tol::kRejected, 0},
    {"array_element_content_nonstring", R"([{"content":123}])", Tol::kRejected, 0},

    // ---- BOM-prefixed (UTF-8 BOM bytes before the array) ----
    {"bom_prefixed", std::string("\xEF\xBB\xBF") + std::string(kArr2), Tol::kParsed, 2},

    // ---- non-object top-level but array recoverable via span ----
    {"prose_with_inner_array",
     R"(The model said: ["not","objects"] which is wrong)", Tol::kRejected, 0},
};

INSTANTIATE_TEST_SUITE_P(ExtractorJson, ExtractorJsonMatrix,
                         ::testing::ValuesIn(kExtractorCases),
                         [](const ::testing::TestParamInfo<ExtractorJsonCase>& i) {
                             return std::string(i.param.name);
                         });

}  // namespace
}  // namespace cortrix::memory
