#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_errors.h"
#include "cortrix/spc/parser_factory.h"
#include "parser_stub.h"

// Error-path coverage for the parser CX_ERR_* identities that had ZERO test
// genuinely triggering them and asserting the surfaced CX_ERR_ envelope code:
//   CX_ERR_UNSUPPORTED_FORMAT  — factory pre-check rejects an unknown extension
//   CX_ERR_ALL_PARSERS_FAILED  — no parser for the route / primary+OCR both fail
//   CX_ERR_ENCODING_ERROR      — bridge reports status=10 (non-UTF-8 conversion)
//   CX_ERR_CORRUPTED_FILE      — bridge reports status=14 (corrupted file)
//   CX_ERR_OUTPUT_TOO_LARGE    — bridge reports status=8 (output over the cap)
//   CX_ERR_EMPTY_DOCUMENT      — bridge reports status=9 (empty doc = OK path)
//
// The three "bridge reports status=N" cases are driven through ParseBridgeJson
// (parser.cpp), the public function that re-inflates the Python bridge's numeric
// status (§2.7 wire form) into a ParsedDoc — a real, in-band trigger of each code
// without needing a live Python subprocess.
namespace cortrix::spc {
namespace {

using test::MakeOnePageDoc;
using test::StubParser;

// RAII temp file with a controllable extension (mirrors test_parser_factory.cpp's
// TempFile) so the factory's stat() pre-check sees a real file.
class TempFile {
public:
    TempFile(const std::string& suffix, size_t bytes) {
        char tmpl[] = "/tmp/cortrix_errpath_XXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd >= 0) ::close(fd);
        path_ = std::string(tmpl) + suffix;
        std::rename(tmpl, path_.c_str());
        std::ofstream f(path_, std::ios::binary);
        f << std::string(bytes, 'x');
    }
    ~TempFile() { std::remove(path_.c_str()); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

// Build a minimal bridge JSON payload that reports a given numeric status (the
// §2.7 ParserError wire value) plus an optional structured_data object — exactly
// what the Python Docling/OCR bridge emits on stdout.
std::string BridgeJsonWithStatus(int status, const nlohmann::json& structured_data) {
    nlohmann::json j;
    j["status"] = status;
    j["error_msg"] = "bridge-reported failure";
    j["structured_data"] = structured_data;
    return j.dump();
}

// ---------------------------------------------------------------------------
// CX_ERR_UNSUPPORTED_FORMAT — factory pre-check on an unknown extension
// ---------------------------------------------------------------------------

TEST(ParserErrPathTest, UnsupportedFormat_SurfacesCxCode) {
    ParserFactoryConfig config;
    DocumentParserFactory factory(config);
    factory.SetPrimaryParser(std::make_unique<StubParser>(
        "docling", std::vector<std::string>{"pdf"}));
    // ".xyz" maps to no MIME type → unsupported-format pre-check fires.
    TempFile bad(".xyz", 32);

    ParsedDoc doc = factory.ParseDocument(bad.path());
    EXPECT_FALSE(doc.ok());
    EXPECT_EQ(doc.status, ParserErrorCode::kUnsupportedFormat);
    EXPECT_EQ(ParserErrorCodeString(doc.status), std::string("CX_ERR_UNSUPPORTED_FORMAT"));
    ASSERT_TRUE(doc.structured_data.is_object());
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_UNSUPPORTED_FORMAT");
    EXPECT_EQ(doc.structured_data["extension"], "xyz");
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kPermanent);
    // The Agent-friendly boundary error carries the same identity.
    auto err = MakeAgentFriendlyError(doc);
    EXPECT_EQ(err.code, "CX_ERR_UNSUPPORTED_FORMAT");
}

// ---------------------------------------------------------------------------
// CX_ERR_ALL_PARSERS_FAILED — (a) no parser registered for the route,
//                             (b) primary + OCR fallback both error
// ---------------------------------------------------------------------------

TEST(ParserErrPathTest, AllParsersFailed_NoParserForRoute_SurfacesCxCode) {
    ParserFactoryConfig config;
    DocumentParserFactory factory(config);  // no primary, no fallback
    TempFile png(".png", 64);               // image route → absent fallback parser

    ParsedDoc doc = factory.ParseDocument(png.path());
    EXPECT_FALSE(doc.ok());
    EXPECT_EQ(doc.status, ParserErrorCode::kAllParsersFailed);
    ASSERT_TRUE(doc.structured_data.is_object());
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_ALL_PARSERS_FAILED");
    ASSERT_TRUE(doc.structured_data["attempted"].is_array());
    EXPECT_TRUE(doc.structured_data["attempted"].empty());
    auto err = MakeAgentFriendlyError(doc);
    EXPECT_EQ(err.code, "CX_ERR_ALL_PARSERS_FAILED");
}

TEST(ParserErrPathTest, AllParsersFailed_PrimaryAndOcrBothFail_SurfacesCxCode) {
    ParserFactoryConfig config;
    DocumentParserFactory factory(config);
    auto p = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    auto f = std::make_unique<StubParser>("paddleocr", std::vector<std::string>{"pdf"});
    StubParser* primary = p.get();
    StubParser* fallback = f.get();
    ParsedDoc primary_err;   primary_err.status = ParserErrorCode::kSubprocessCrashed;
    ParsedDoc ocr_err;       ocr_err.status = ParserErrorCode::kOcrFailed;
    primary->SetResult(primary_err);
    fallback->SetResult(ocr_err);
    factory.SetPrimaryParser(std::move(p));
    factory.SetFallbackParser(std::move(f));

    TempFile pdf(".pdf", 1024);
    ParsedDoc doc = factory.ParseDocument(pdf.path());
    EXPECT_EQ(doc.status, ParserErrorCode::kAllParsersFailed);
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_ALL_PARSERS_FAILED");
    ASSERT_TRUE(doc.structured_data["attempted"].is_array());
    EXPECT_EQ(doc.structured_data["attempted"].size(), 2u);
}

// ---------------------------------------------------------------------------
// CX_ERR_ENCODING_ERROR — bridge reports status=10 (kEncodingError)
// ---------------------------------------------------------------------------

TEST(ParserErrPathTest, EncodingError_FromBridgeStatus_SurfacesCxCode) {
    // The bridge attaches the §5.2 required key (detected_encoding) on a non-UTF-8
    // file whose conversion failed.
    const std::string bridge =
        BridgeJsonWithStatus(static_cast<int>(ParserErrorCode::kEncodingError),
                             {{"detected_encoding", "GB18030"}});
    ParsedDoc doc = ParseBridgeJson(bridge, "/tmp/cjk.txt", "docling");

    EXPECT_FALSE(doc.ok());
    EXPECT_EQ(doc.status, ParserErrorCode::kEncodingError);
    EXPECT_EQ(ParserErrorCodeString(doc.status), std::string("CX_ERR_ENCODING_ERROR"));
    ASSERT_TRUE(doc.structured_data.is_object());
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_ENCODING_ERROR");
    EXPECT_EQ(doc.structured_data["detected_encoding"], "GB18030");
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kPermanent);
    auto err = MakeAgentFriendlyError(doc);
    EXPECT_EQ(err.code, "CX_ERR_ENCODING_ERROR");
}

// ---------------------------------------------------------------------------
// CX_ERR_CORRUPTED_FILE — bridge reports status=14 (kCorruptedFile)
// ---------------------------------------------------------------------------

TEST(ParserErrPathTest, CorruptedFile_FromBridgeStatus_SurfacesCxCode) {
    const std::string bridge =
        BridgeJsonWithStatus(static_cast<int>(ParserErrorCode::kCorruptedFile),
                             {{"magic_bytes_expected", "25504446"},  // "%PDF"
                              {"magic_bytes_actual", "deadbeef"}});
    ParsedDoc doc = ParseBridgeJson(bridge, "/tmp/garbage.pdf", "docling");

    EXPECT_FALSE(doc.ok());
    EXPECT_EQ(doc.status, ParserErrorCode::kCorruptedFile);
    EXPECT_EQ(ParserErrorCodeString(doc.status), std::string("CX_ERR_CORRUPTED_FILE"));
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_CORRUPTED_FILE");
    EXPECT_EQ(doc.structured_data["magic_bytes_actual"], "deadbeef");
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kPermanent);
    auto err = MakeAgentFriendlyError(doc);
    EXPECT_EQ(err.code, "CX_ERR_CORRUPTED_FILE");
}

// ---------------------------------------------------------------------------
// CX_ERR_OUTPUT_TOO_LARGE — bridge reports status=8 (kOutputTooLarge)
// ---------------------------------------------------------------------------

TEST(ParserErrPathTest, OutputTooLarge_FromBridgeStatus_SurfacesCxCode) {
    const std::string bridge =
        BridgeJsonWithStatus(static_cast<int>(ParserErrorCode::kOutputTooLarge),
                             {{"actual_bytes", 99000000}, {"limit_bytes", 52428800}});
    ParsedDoc doc = ParseBridgeJson(bridge, "/tmp/huge.pdf", "docling");

    EXPECT_FALSE(doc.ok());
    EXPECT_EQ(doc.status, ParserErrorCode::kOutputTooLarge);
    EXPECT_EQ(ParserErrorCodeString(doc.status), std::string("CX_ERR_OUTPUT_TOO_LARGE"));
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_OUTPUT_TOO_LARGE");
    EXPECT_EQ(doc.structured_data["limit_bytes"], 52428800);
    // §5.2 / registry: OUTPUT_TOO_LARGE is a quota-category, non-retryable fault.
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kQuota);
    auto err = MakeAgentFriendlyError(doc);
    EXPECT_EQ(err.code, "CX_ERR_OUTPUT_TOO_LARGE");
}

// ---------------------------------------------------------------------------
// CX_ERR_EMPTY_DOCUMENT — bridge reports status=9. §5.1: an empty document is NOT
// an error (status=OK, pages=[]) but it carries the CX_ERR_EMPTY_DOCUMENT identity
// in the registry. ParseBridgeJson keeps the kEmptyDocument status on the OK path.
// ---------------------------------------------------------------------------

TEST(ParserErrPathTest, EmptyDocument_FromBridgeStatus_IsOkButCarriesCxCode) {
    nlohmann::json j;
    j["status"] = static_cast<int>(ParserErrorCode::kEmptyDocument);  // 9
    j["parser"] = "docling";
    j["pages"] = nlohmann::json::array();  // empty document
    ParsedDoc doc = ParseBridgeJson(j.dump(), "/tmp/blank.pdf", "docling");

    // §5.1: empty document is treated as success (IsParserOk(kEmptyDocument) == true).
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(doc.status, ParserErrorCode::kEmptyDocument);
    EXPECT_TRUE(doc.pages.empty());
    // The registry maps the status to the stable CX_ERR_ string + permanent category.
    EXPECT_EQ(ParserErrorCodeString(ParserErrorCode::kEmptyDocument),
              std::string("CX_ERR_EMPTY_DOCUMENT"));
    EXPECT_TRUE(IsParserOk(ParserErrorCode::kEmptyDocument));
}

}  // namespace
}  // namespace cortrix::spc
