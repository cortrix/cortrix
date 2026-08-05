#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/parser.h"

// Parser branch coverage for parser.cpp: the JSON-field default branches in
// ParseChunk / ParsePageMeta / ParseDocMeta (taken when a field is ABSENT vs
// present), StatusToCode out-of-range mapping, MakeErrorDoc structured_data
// edge cases, and MakeAgentFriendlyError with a non-object structured_data.
// Pure functions, no subprocess — these complement the success-path coverage in
// test_docling_parser.cpp (which exercises the present-field branches).
namespace cortrix::spc {
namespace {

// ParseBridgeJson on a success envelope whose pages/paragraphs/page_metadata omit
// every optional field → every j.value(...) default branch is taken.
TEST(ParserBranchesTest, AllOptionalFieldsAbsentTakeDefaults) {
    // status omitted (defaults to 0 = OK); one page + one paragraph, each a bare
    // object so ParseChunk / ParsePageMeta / ParseDocMeta all fall to defaults.
    const char* json = R"JSON({
      "pages": [{"paragraphs": [{}]}]
    })JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/uploads/some/deep/path.bin", "myparser");
    ASSERT_TRUE(doc.ok());
    // parser absent → falls back to the passed-in parser_name.
    EXPECT_EQ(doc.parser_name, "myparser");
    // filename absent → basename of the input path.
    EXPECT_EQ(doc.metadata.filename, "path.bin");
    // doc-meta numeric defaults.
    EXPECT_EQ(doc.metadata.page_count, -1);
    EXPECT_EQ(doc.metadata.file_size_bytes, 0);
    EXPECT_TRUE(doc.metadata.doc_language.empty());
    ASSERT_EQ(doc.pages.size(), 1u);
    EXPECT_EQ(doc.pages[0].page_num, 0);          // page_num absent → 0
    EXPECT_TRUE(doc.pages[0].page_text.empty());
    ASSERT_EQ(doc.pages[0].paragraphs.size(), 1u);
    const ParsedChunk& c = doc.pages[0].paragraphs[0];
    EXPECT_TRUE(c.text.empty());
    EXPECT_EQ(c.page, -1);                          // page absent → -1
    EXPECT_TRUE(c.section.empty());
    EXPECT_EQ(c.type, ChunkType::TEXT);            // type absent → TEXT default
    EXPECT_FLOAT_EQ(c.confidence, 0.0f);
    EXPECT_EQ(c.char_offset, -1);
    EXPECT_EQ(c.char_length, -1);
    // page_metadata absent → ParsePageMeta defaults (page_num falls to the page's).
    EXPECT_FALSE(doc.pages[0].page_metadata.is_scan_page);
    EXPECT_EQ(doc.pages[0].page_metadata.char_count, 0);
}

// metadata.filename present and non-empty → the basename fallback branch is NOT
// taken (the present-value branch wins).
TEST(ParserBranchesTest, FilenamePresentSkipsBasenameFallback) {
    const char* json = R"JSON({"status":0,"metadata":{"filename":"explicit.pdf"},"pages":[]})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/var/ignored/path.bin", "p");
    EXPECT_EQ(doc.metadata.filename, "explicit.pdf");
}

// A path with no slash → the basename fallback uses the whole path (find_last_of
// returns npos branch).
TEST(ParserBranchesTest, NoSlashPathBasenameFallback) {
    const char* json = R"JSON({"status":0,"metadata":{},"pages":[]})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "bare_filename.txt", "p");
    EXPECT_EQ(doc.metadata.filename, "bare_filename.txt");
}

// "pages" present but NOT an array → the j["pages"].is_array() guard is false, so
// the page loop is skipped (the false branch). Same for failed_pages.
TEST(ParserBranchesTest, PagesNotArraySkipped) {
    const char* json = R"JSON({"status":0,"pages":"oops","failed_pages":42})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/x", "p");
    ASSERT_TRUE(doc.ok());
    EXPECT_TRUE(doc.pages.empty());
    EXPECT_TRUE(doc.failed_pages.empty());
}

// failed_pages array with a non-integer entry → the is_number_integer() guard
// rejects it (false branch), keeping only the integer entries.
TEST(ParserBranchesTest, FailedPagesNonIntegerEntriesFiltered) {
    const char* json = R"JSON({"status":0,"pages":[],"failed_pages":[1,"two",3.5,4]})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/x", "p");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc.failed_pages, (std::vector<int>{1, 4}));
}

// A page missing "paragraphs" entirely → the paragraphs is_array() guard false
// branch (page produced with zero paragraphs).
TEST(ParserBranchesTest, PageWithoutParagraphs) {
    const char* json = R"JSON({"status":0,"pages":[{"page_num":7,"page_text":"x"}]})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/x", "p");
    ASSERT_TRUE(doc.ok());
    ASSERT_EQ(doc.pages.size(), 1u);
    EXPECT_EQ(doc.pages[0].page_num, 7);
    EXPECT_TRUE(doc.pages[0].paragraphs.empty());
}

// A bridge error envelope whose structured_data is present but NOT an object →
// ParseBridgeJson resets it to an empty object (the !is_object() branch).
TEST(ParserBranchesTest, ErrorEnvelopeNonObjectStructuredDataReset) {
    // status 13 = PASSWORD_PROTECTED (a real error code); structured_data is an
    // array, which must be coerced to {} (then MakeErrorDoc stamps "code").
    const char* json = R"JSON({"status":13,"error_msg":"locked","structured_data":[1,2]})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/x", "p");
    EXPECT_EQ(doc.status, ParserErrorCode::kPasswordProtected);
    ASSERT_TRUE(doc.structured_data.is_object());
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_PASSWORD_PROTECTED");
}

// status int out of the enum range → StatusToCode maps it to INVALID_OUTPUT
// (the < 0 || > kMaxPagesExceeded branch).
TEST(ParserBranchesTest, OutOfRangeStatusMapsToInvalidOutput) {
    const char* json = R"JSON({"status":9999,"error_msg":"weird"})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/x", "p");
    EXPECT_EQ(doc.status, ParserErrorCode::kInvalidOutput);
}

TEST(ParserBranchesTest, NegativeStatusMapsToInvalidOutput) {
    const char* json = R"JSON({"status":-5})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/x", "p");
    EXPECT_EQ(doc.status, ParserErrorCode::kInvalidOutput);
}

// MakeErrorDoc: a structured_data that already carries "code" must NOT be
// overwritten (the !contains("code") branch is false).
TEST(ParserBranchesTest, MakeErrorDocKeepsExplicitCode) {
    nlohmann::json sd = {{"code", "CX_CUSTOM_OVERRIDE"}, {"k", 1}};
    ParsedDoc doc = MakeErrorDoc(ParserErrorCode::kSubprocessFailed, "boom", sd, "p");
    EXPECT_EQ(doc.structured_data["code"], "CX_CUSTOM_OVERRIDE");
    EXPECT_EQ(doc.structured_data["k"], 1);
}

// MakeErrorDoc: a non-object structured_data (e.g. the default null) skips the
// code-stamping entirely (is_object() false branch) and is stored as-is.
TEST(ParserBranchesTest, MakeErrorDocNonObjectStructuredDataNotStamped) {
    ParsedDoc doc = MakeErrorDoc(ParserErrorCode::kSubprocessFailed, "boom",
                                 nlohmann::json(nullptr), "p");
    EXPECT_FALSE(doc.structured_data.is_object());
    EXPECT_EQ(doc.status, ParserErrorCode::kSubprocessFailed);
    EXPECT_FALSE(doc.retryable);
}

// MakeAgentFriendlyError with a doc whose structured_data is NOT an object → the
// rebuild substitutes an empty object (the ternary's false branch).
TEST(ParserBranchesTest, MakeAgentFriendlyErrorNonObjectStructuredData) {
    ParsedDoc doc;
    doc.status = ParserErrorCode::kParseTimeout;
    doc.error_msg = "took too long";
    doc.structured_data = nlohmann::json::array({1, 2, 3});  // not an object
    auto err = MakeAgentFriendlyError(doc);
    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_PARSE_TIMEOUT");
}

// ChunkTypeFromString covers all four non-default tokens + the default; ToString
// covers the full switch. (Round-trip is in test_parser_interface; here we pin
// the individual string arms explicitly so each case label is exercised.)
TEST(ParserBranchesTest, ChunkTypeFromStringEveryArm) {
    EXPECT_EQ(ChunkTypeFromString("TABLE"), ChunkType::TABLE);
    EXPECT_EQ(ChunkTypeFromString("IMAGE_CAPTION"), ChunkType::IMAGE_CAPTION);
    EXPECT_EQ(ChunkTypeFromString("CODE"), ChunkType::CODE);
    EXPECT_EQ(ChunkTypeFromString("META"), ChunkType::META);
    EXPECT_EQ(ChunkTypeFromString("TEXT"), ChunkType::TEXT);
    EXPECT_EQ(ChunkTypeFromString(""), ChunkType::TEXT);  // unknown → default
}

TEST(ParserBranchesTest, ToStringEveryArm) {
    EXPECT_STREQ(ToString(ChunkType::TEXT), "TEXT");
    EXPECT_STREQ(ToString(ChunkType::TABLE), "TABLE");
    EXPECT_STREQ(ToString(ChunkType::IMAGE_CAPTION), "IMAGE_CAPTION");
    EXPECT_STREQ(ToString(ChunkType::CODE), "CODE");
    EXPECT_STREQ(ToString(ChunkType::META), "META");
}

// IDocumentParser::IsFormatSupported normalization branches: a leading dot is
// stripped, uppercase is lowercased, and an unknown ext returns false. Uses a
// tiny inline parser so the base-class method is exercised directly.
namespace {
class FmtStub : public IDocumentParser {
public:
    ParsedDoc Parse(const std::string&, const ParserOptions&) override { return {}; }
    std::vector<std::string> SupportedFormats() const override { return {"pdf", "txt"}; }
    const char* Name() const override { return "fmtstub"; }
};
}  // namespace

TEST(ParserBranchesTest, IsFormatSupportedNormalizes) {
    FmtStub s;
    EXPECT_TRUE(s.IsFormatSupported(".PDF"));   // leading dot + uppercase
    EXPECT_TRUE(s.IsFormatSupported("txt"));
    EXPECT_FALSE(s.IsFormatSupported("docx"));  // not in the list
    EXPECT_FALSE(s.IsFormatSupported(""));       // empty (front() guard false branch)
}

// DrivePageProgress early-returns: null callback (false on the !opts.on_page_progress
// guard) and a doc-level failure (the !doc.ok() guard). Neither must invoke the
// callback. (The success ordering path is covered in test_parser_page_progress.cpp.)
TEST(ParserBranchesTest, DrivePageProgressNullCallbackNoOp) {
    ParsedDoc doc;
    doc.status = ParserErrorCode::kOk;
    ParsedPage pg;
    pg.page_num = 1;
    doc.pages.push_back(pg);
    ParserOptions opts;  // on_page_progress = nullptr
    DrivePageProgress(doc, opts);  // must not crash / must early-return
    SUCCEED();
}

TEST(ParserBranchesTest, DrivePageProgressErrorDocNoCallback) {
    ParsedDoc doc;
    doc.status = ParserErrorCode::kSubprocessFailed;  // !ok() → early return
    ParsedPage pg;
    pg.page_num = 1;
    doc.pages.push_back(pg);
    int calls = 0;
    ParserOptions opts;
    opts.on_page_progress = [&](int, int, bool) { ++calls; };
    DrivePageProgress(doc, opts);
    EXPECT_EQ(calls, 0);
}

}  // namespace
}  // namespace cortrix::spc
