// JSON parse-tolerance matrix for DocSummaryGenerator::ParseStructuredOutput (F41 §9.1).
//
// Real parser (src/doc_summary/doc_summary_generator.cpp ParseStructuredOutput):
//   1. json::parse(allow_exceptions=false). discarded OR not object -> error Result
//      (DocSummaryStatus kLlmInvalidOutput; message carries
//      "CX_ERR_F41_LLM_INVALID_OUTPUT").  *** NO fence / wrapper / span recovery. ***
//   2. "summary_text" REQUIRED + must be a string, else error.
//   3. summary_text is UTF-8-truncated to max_chars; keywords/topics pulled via
//      ToStringArray (missing/non-array -> empty; non-string elements skipped);
//      one_liner optional string.
//
// Tolerance is strict like the judgment parser: only a clean JSON object with a
// string summary_text parses; fenced / prose-wrapped / object-wrapped / truncated
// inputs are gracefully rejected (no crash). The generator is constructed with a
// MockLlmClient + MockChunkStore (ParseStructuredOutput itself uses neither).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/doc_summary/doc_summary_generator.h"
#include "cortrix/doc_summary/doc_summary_types.h"
#include "mock_chunk_store.h"
#include "mock_llm_client.h"

namespace cortrix::doc_summary {
namespace {

enum class DTol {
    kParsed,    // object with string summary_text -> parses
    kRejected,  // graceful error Result (CX_ERR_F41_LLM_INVALID_OUTPUT)
};

struct DocSummaryJsonCase {
    const char* name;
    std::string input;
    DTol expect;
    int min_summary_len;   // when parsed: summary_text must be at least this long (chars)
    int expected_keywords; // when parsed: keyword count (-1 = don't check)
};

DocSummaryGenerator MakeDocSummaryParser() {
    return DocSummaryGenerator(DocSummaryConfig{},
                               std::make_shared<llm::MockLlmClient>(),
                               std::make_shared<store::MockChunkStore>());
}

class DocSummaryJsonMatrix : public ::testing::TestWithParam<DocSummaryJsonCase> {};

TEST_P(DocSummaryJsonMatrix, Tolerance) {
    const DocSummaryJsonCase& tc = GetParam();
    DocSummaryGenerator g = MakeDocSummaryParser();
    Result<DocSummaryStructured> r = g.ParseStructuredOutput(tc.input, /*max_chars=*/500);
    if (tc.expect == DTol::kParsed) {
        ASSERT_TRUE(r.ok()) << tc.name << " status=" << r.status().message();
        const DocSummaryStructured& s = r.value();
        EXPECT_GE(static_cast<int>(s.summary_text.size()), tc.min_summary_len) << tc.name;
        if (tc.expected_keywords >= 0) {
            EXPECT_EQ(static_cast<int>(s.keywords.size()), tc.expected_keywords) << tc.name;
        }
    } else {
        ASSERT_FALSE(r.ok()) << tc.name << " unexpectedly parsed";
        EXPECT_NE(r.status().message().find("CX_ERR_F41_LLM_INVALID_OUTPUT"),
                  std::string::npos)
            << tc.name << " msg=" << r.status().message();
    }
}

// A well-formed structured summary object reused inside wrappers.
const char* kGood =
    R"({"summary_text":"Quarterly revenue grew across all segments.",)"
    R"("keywords":["revenue","growth","segments"],)"
    R"("topics":["Financial Report"],"one_liner":"Revenue up"})";

const std::vector<DocSummaryJsonCase> kDocSummaryCases = {
    // ---- clean valid ----
    {"clean_full", std::string(kGood), DTol::kParsed, 10, 3},
    {"clean_summary_only", R"({"summary_text":"hello world"})", DTol::kParsed, 5, 0},
    {"clean_keywords_nonstring_skipped",
     R"({"summary_text":"x text here","keywords":["a",2,"b"]})",
     DTol::kParsed, 5, 2},
    {"clean_keywords_not_array",
     R"({"summary_text":"x text here","keywords":"oops"})", DTol::kParsed, 5, 0},

    // ---- ```json fenced -> NOT an object, rejected ----
    {"fenced_json", "```json\n" + std::string(kGood) + "\n```", DTol::kRejected, 0, -1},
    // ---- ``` bare-fenced -> rejected ----
    {"fenced_bare", "```\n" + std::string(kGood) + "\n```", DTol::kRejected, 0, -1},

    // ---- object-wrapped (top-level lacks summary_text) ----
    {"wrapped_result", R"({"result":)" + std::string(kGood) + "}", DTol::kRejected, 0, -1},
    {"wrapped_summary", R"({"summary":)" + std::string(kGood) + "}", DTol::kRejected, 0, -1},

    // ---- prose-surrounded -> rejected ----
    {"prose_before", "Here is your summary:\n" + std::string(kGood), DTol::kRejected, 0, -1},
    {"prose_after", std::string(kGood) + "\nLet me know if you need more.", DTol::kRejected, 0, -1},

    // ---- leading/trailing whitespace: nlohmann tolerates ----
    {"ws_leading", "   \n\t" + std::string(kGood), DTol::kParsed, 10, 3},
    {"ws_trailing", std::string(kGood) + "\n\n  ", DTol::kParsed, 10, 3},

    // ---- NaN / Infinity literals -> non-standard -> reject ----
    {"nan_in_object",
     R"({"summary_text":"x text here","score":NaN})", DTol::kRejected, 0, -1},
    {"infinity_in_object",
     R"({"summary_text":"x text here","score":Infinity})", DTol::kRejected, 0, -1},

    // ---- duplicate keys: nlohmann keeps last; still valid object ----
    {"duplicate_summary_text",
     R"({"summary_text":"first one","summary_text":"second longer text"})",
     DTol::kParsed, 5, 0},

    // ---- truncated ----
    {"truncated_object", R"({"summary_text":"x")", DTol::kRejected, 0, -1},
    {"truncated_value", R"({"summary_text":"x)", DTol::kRejected, 0, -1},

    // ---- empty string ----
    {"empty_string", "", DTol::kRejected, 0, -1},
    {"whitespace_only", "   \n  ", DTol::kRejected, 0, -1},

    // ---- non-object-when-object-expected ----
    {"array_top_level", "[" + std::string(kGood) + "]", DTol::kRejected, 0, -1},
    {"scalar_number", "42", DTol::kRejected, 0, -1},
    {"scalar_string", R"("a summary")", DTol::kRejected, 0, -1},
    {"json_null", "null", DTol::kRejected, 0, -1},

    // ---- object but missing / wrong-typed summary_text ----
    {"missing_summary_text", R"({"keywords":["a","b"]})", DTol::kRejected, 0, -1},
    {"summary_text_number", R"({"summary_text":123})", DTol::kRejected, 0, -1},
    {"summary_text_array", R"({"summary_text":["a"]})", DTol::kRejected, 0, -1},
    {"summary_text_null", R"({"summary_text":null})", DTol::kRejected, 0, -1},

    // ---- BOM-prefixed: nlohmann skips leading UTF-8 BOM -> parses ----
    {"bom_prefixed",
     std::string("\xEF\xBB\xBF") + std::string(kGood), DTol::kParsed, 10, 3},

    // ---- nested-bracket / brace inside summary string is fine ----
    {"nested_bracket_in_summary",
     R"({"summary_text":"covers set {a,b} and list [1,2,3] in prose form"})",
     DTol::kParsed, 10, 0},
};

INSTANTIATE_TEST_SUITE_P(DocSummaryJson, DocSummaryJsonMatrix,
                         ::testing::ValuesIn(kDocSummaryCases),
                         [](const ::testing::TestParamInfo<DocSummaryJsonCase>& i) {
                             return std::string(i.param.name);
                         });

}  // namespace
}  // namespace cortrix::doc_summary
