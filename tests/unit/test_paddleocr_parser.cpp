#include <gtest/gtest.h>

#include <string>

#include "cortrix/spc/paddleocr_parser.h"
#include "cortrix/spc/parser.h"
#include "parser_script_fixture.h"

// Parser S3 coverage: PaddleOCRParser subprocess wrapper. Standalone — exercised
// against a mock bridge (paddleocr need not be installed). Real OCR is integration.
namespace cortrix::spc {
namespace {

using test::ScriptFixture;

class PaddleOCRParserTest : public ScriptFixture {
protected:
    PaddleOCRParserConfig CfgFor(const std::string& script) {
        PaddleOCRParserConfig cfg;
        cfg.python_path = "python3";
        cfg.script_path = script;
        cfg.subprocess_timeout_ms = 5000;
        return cfg;
    }
};

// A OCR success payload (one scan page, all TEXT, empty section).
const char* kOcrJson = R"JSON({
  "status": 0, "parser": "paddleocr",
  "metadata": {"filename": "scan.pdf", "page_count": 1, "doc_language": "zh"},
  "pages": [{"page_num": 1, "page_text": "scanned text",
             "page_metadata": {"page_num": 1, "parser_used": "paddleocr",
                               "page_confidence": 0.82, "is_scan_page": true,
                               "char_count": 4},
             "paragraphs": [{"text": "scanned text", "page": 1, "section": "",
                             "type": "TEXT", "confidence": 0.82, "language": "zh",
                             "char_offset": -1, "char_length": 4}]}],
  "failed_pages": [], "error_msg": "", "retryable": false,
  "category": "NONE", "retry_after_ms": 0, "structured_data": {}
})JSON";

TEST_F(PaddleOCRParserTest, ScanPDF_Success) {
    PaddleOCRParser p(CfgFor(MockEmitting(kOcrJson)));
    ParsedDoc doc = p.Parse("/tmp/scan.pdf");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc.parser_name, "paddleocr");
    ASSERT_EQ(doc.pages.size(), 1u);
    EXPECT_TRUE(doc.pages[0].page_metadata.is_scan_page);
    ASSERT_EQ(doc.pages[0].paragraphs.size(), 1u);
    // OCR paragraphs are TEXT with empty section.
    EXPECT_EQ(doc.pages[0].paragraphs[0].type, ChunkType::TEXT);
    EXPECT_EQ(doc.pages[0].paragraphs[0].section, "");
}

TEST_F(PaddleOCRParserTest, Image_Success) {
    PaddleOCRParser p(CfgFor(MockEmitting(kOcrJson)));
    std::string png = Write(".png", "\x89PNG fake");
    ParsedDoc doc = p.Parse(png);
    EXPECT_TRUE(doc.ok());
}

//: non-PDF/non-image formats are rejected before any subprocess spawn.
TEST_F(PaddleOCRParserTest, UnsupportedFormat_NoSpawn) {
    // Point at a script that would crash if run, to prove it's never invoked.
    PaddleOCRParser p(CfgFor(Write(".py", "import sys\nsys.exit(2)\n")));
    ParsedDoc doc = p.Parse("/tmp/document.docx");
    EXPECT_EQ(doc.status, ParserErrorCode::kUnsupportedFormat);
    EXPECT_EQ(doc.structured_data["extension"], "docx");
}

TEST_F(PaddleOCRParserTest, Timeout) {
    PaddleOCRParserConfig cfg = CfgFor(Write(".py", "import time\ntime.sleep(10)\n"));
    cfg.subprocess_timeout_ms = 300;
    PaddleOCRParser p(cfg);
    ParserOptions opts;
    opts.subprocess_timeout_ms = 300;
    ParsedDoc doc = p.Parse("/tmp/scan.pdf", opts);
    EXPECT_EQ(doc.status, ParserErrorCode::kParseTimeout);
    EXPECT_TRUE(doc.retryable);
}

// An engine crash (non-zero exit) maps to OCR_FAILED (retryable) — the
// actionable signal for the Agent.
TEST_F(PaddleOCRParserTest, Crash_MapsToOcrFailed) {
    PaddleOCRParser p(CfgFor(Write(".py",
        "import sys\nsys.stderr.write('ocr boom\\n')\nsys.exit(1)\n")));
    ParsedDoc doc = p.Parse("/tmp/scan.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kOcrFailed);
    EXPECT_TRUE(doc.retryable);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kTransient);
    EXPECT_NE(doc.structured_data["stderr_tail"].get<std::string>().find("boom"),
              std::string::npos);
}

TEST_F(PaddleOCRParserTest, BuildArgv_IncludesLangAndGpu) {
    PaddleOCRParserConfig cfg = CfgFor("/path/paddleocr_bridge.py");
    cfg.lang = "ch";
    cfg.use_gpu = true;
    PaddleOCRParser p(cfg);
    auto argv = p.BuildArgv("/tmp/scan.pdf", ParserOptions{});
    auto has = [&](const std::string& tok) {
        for (const auto& a : argv) if (a == tok) return true;
        return false;
    };
    EXPECT_TRUE(has("--lang"));
    EXPECT_TRUE(has("ch"));
    EXPECT_TRUE(has("--use-gpu"));
    EXPECT_TRUE(has("--filepath"));
}

TEST_F(PaddleOCRParserTest, SupportedFormats) {
    PaddleOCRParser p(CfgFor("x"));
    EXPECT_TRUE(p.IsFormatSupported("pdf"));
    EXPECT_TRUE(p.IsFormatSupported("png"));
    EXPECT_FALSE(p.IsFormatSupported("docx"));
}

// --- branch coverage: error envelopes + format gate + argv variants ---

// A missing python binary → the subprocess never launches → SUBPROCESS_FAILED
// with the install hint (the !sp.launched branch).
TEST_F(PaddleOCRParserTest, LaunchFailure_SubprocessFailed) {
    PaddleOCRParserConfig cfg = CfgFor("/nonexistent/ocr_bridge.py");
    cfg.python_path = "/nonexistent/python_binary_xyz";
    PaddleOCRParser p(cfg);
    ParsedDoc doc = p.Parse("/tmp/scan.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kSubprocessFailed);
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_SUBPROCESS_FAILED");
    EXPECT_NE(doc.structured_data["hint"].get<std::string>().find("paddleocr"),
              std::string::npos);
}

// stdout exceeds the cap → the bridge is killed mid-stream → OUTPUT_TOO_LARGE
// (the sp.output_truncated branch) with the quota category.
TEST_F(PaddleOCRParserTest, OutputTooLarge_Quota) {
    PaddleOCRParserConfig cfg = CfgFor(Write(".py", "print('x' * 200000)\n"));
    cfg.max_output_size_bytes = 4096;
    PaddleOCRParser p(cfg);
    ParsedDoc doc = p.Parse("/tmp/scan.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kOutputTooLarge);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kQuota);
    EXPECT_EQ(doc.structured_data["limit_bytes"], 4096);
}

// The OcrSupports gate accepts every image/PDF variant (jpeg / tif / bmp arms)
// and IsFormatSupported reflects the same set.
TEST_F(PaddleOCRParserTest, SupportedFormats_AllImageVariants) {
    PaddleOCRParser p(CfgFor("x"));
    for (const char* ext : {"pdf", "png", "jpg", "jpeg", "tiff", "tif", "bmp"}) {
        EXPECT_TRUE(p.IsFormatSupported(ext)) << ext;
    }
    EXPECT_FALSE(p.IsFormatSupported("gif"));  // not an OCR format
}

// A jpeg input parses through the OcrSupports("jpeg") arm (distinct from the pdf
// arm covered by ScanPDF_Success).
TEST_F(PaddleOCRParserTest, JpegInput_Success) {
    PaddleOCRParser p(CfgFor(MockEmitting(kOcrJson)));
    std::string jpg = Write(".jpeg", "fake jpeg");
    ParsedDoc doc = p.Parse(jpg);
    EXPECT_TRUE(doc.ok());
}

// A file path with no extension → LowerExt returns "" → OcrSupports("") is false
// → UNSUPPORTED_FORMAT (the dot-not-found branch of LowerExt).
TEST_F(PaddleOCRParserTest, NoExtension_Unsupported) {
    PaddleOCRParser p(CfgFor("x"));
    ParsedDoc doc = p.Parse("/tmp/noextfile");
    EXPECT_EQ(doc.status, ParserErrorCode::kUnsupportedFormat);
    EXPECT_EQ(doc.structured_data["extension"], "");
}

// BuildArgv without GPU omits the --use-gpu flag (the config_.use_gpu false
// branch), and lang defaults flow through.
TEST_F(PaddleOCRParserTest, BuildArgv_NoGpuOmitsFlag) {
    PaddleOCRParserConfig cfg = CfgFor("/path/paddleocr_bridge.py");
    cfg.use_gpu = false;
    PaddleOCRParser p(cfg);
    auto argv = p.BuildArgv("/tmp/scan.pdf", ParserOptions{});
    bool has_gpu = false;
    for (const auto& a : argv) if (a == "--use-gpu") has_gpu = true;
    EXPECT_FALSE(has_gpu);
}

// The per-request timeout only overrides the config timeout when it is BOTH > 0
// AND smaller. A request timeout of 0 leaves the config value in force (the
// opts.subprocess_timeout_ms > 0 guard false branch) → a successful parse, no
// premature kill.
TEST_F(PaddleOCRParserTest, RequestTimeoutZeroKeepsConfigTimeout) {
    PaddleOCRParserConfig cfg = CfgFor(MockEmitting(kOcrJson));
    cfg.subprocess_timeout_ms = 5000;
    PaddleOCRParser p(cfg);
    ParserOptions opts;
    opts.subprocess_timeout_ms = 0;  // does not override
    ParsedDoc doc = p.Parse("/tmp/scan.pdf", opts);
    EXPECT_TRUE(doc.ok());
}

// End-to-end against the real paddleocr_bridge.py (paddleocr likely absent →
// SUBPROCESS_FAILED). Proves the C++↔Python contract; real OCR is integration.
TEST_F(PaddleOCRParserTest, RealBridge_ContractHolds) {
    const char* candidates[] = {
        "scripts/paddleocr_bridge.py",
        "../scripts/paddleocr_bridge.py",
        "../../scripts/paddleocr_bridge.py",
    };
    std::string script;
    for (const char* c : candidates) {
        std::ifstream f(c);
        if (f.good()) { script = c; break; }
    }
    if (script.empty()) GTEST_SKIP() << "paddleocr_bridge.py not found from CWD";

    std::string pdf = Write(".pdf", "%PDF-1.4 stub\n");
    PaddleOCRParser p(CfgFor(script));
    ParsedDoc doc = p.Parse(pdf);
    // The bridge always emits a well-formed envelope (exit 0 on expected paths).
    EXPECT_NE(doc.status, ParserErrorCode::kInvalidOutput);
    if (doc.status == ParserErrorCode::kSubprocessFailed) {
        EXPECT_EQ(doc.structured_data["code"], "CX_ERR_SUBPROCESS_FAILED");
    }
}

}  // namespace
}  // namespace cortrix::spc
