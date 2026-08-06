#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cortrix/spc/docling_parser.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_subprocess.h"

// Parser S2 coverage: DoclingParser subprocess wrapper + ParseBridgeJson (§3.1).
//
// Standalone strategy (briefing): the dev machine has python3 but not
// necessarily docling, so the wrapper logic is exercised against *mock* bridge
// scripts (canned JSON / sleep / crash / bad output). Real docling end-to-end
// is D3.5. ParseBridgeJson is also tested directly with no subprocess.
namespace cortrix::spc {
namespace {

// Write `content` to a unique temp file with `suffix`; returns its path. The
// file is left for the test process lifetime (cleaned in TearDown via paths_).
class ScriptEnv : public ::testing::Test {
protected:
    std::string Write(const std::string& suffix, const std::string& content,
                      bool executable = false) {
        char tmpl[] = "/tmp/cortrix_docling_XXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd >= 0) ::close(fd);
        std::string path = std::string(tmpl) + suffix;
        std::rename(tmpl, path.c_str());
        std::ofstream f(path);
        f << content;
        f.close();
        if (executable) ::chmod(path.c_str(), 0755);
        paths_.push_back(path);
        return path;
    }

    // A python mock bridge that ignores args and prints `json_literal` verbatim.
    std::string MockEmitting(const std::string& json_literal) {
        std::string body =
            "import sys\n"
            "print('''" + json_literal + "''')\n";
        return Write(".py", body);
    }

    void TearDown() override {
        for (const auto& p : paths_) std::remove(p.c_str());
    }

    DoclingParserConfig CfgFor(const std::string& script) {
        DoclingParserConfig cfg;
        cfg.python_path = "python3";
        cfg.script_path = script;
        cfg.subprocess_timeout_ms = 5000;
        return cfg;
    }

    std::vector<std::string> paths_;
};

// A realistic §3.1 success payload (single text page).
const char* kSuccessJson = R"JSON({
  "status": 0, "parser": "docling",
  "metadata": {"filename": "report.pdf", "doc_title": "Q1 Report",
               "mime_type": "application/pdf", "file_size_bytes": 2048576,
               "page_count": 1, "doc_language": "en",
               "upload_timestamp": 1716220800000, "parse_time_ms": 1850},
  "pages": [{"page_num": 1, "page_text": "Hello world",
             "page_metadata": {"page_num": 1, "parser_used": "docling",
                               "page_confidence": 0.93, "is_scan_page": false,
                               "char_count": 11},
             "paragraphs": [{"text": "Hello world", "page": 1, "section": "Intro",
                             "type": "TEXT", "confidence": 0.95, "language": "en",
                             "char_offset": 0, "char_length": 11}]}],
  "failed_pages": [], "error_msg": "", "retryable": false,
  "category": "NONE", "retry_after_ms": 0, "structured_data": {}
})JSON";

// --- subprocess wrapper success ---

TEST_F(ScriptEnv, Docling_Parse_Success) {
    DoclingParser p(CfgFor(MockEmitting(kSuccessJson)));
    ParsedDoc doc = p.Parse("/tmp/report.pdf");
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(doc.parser_name, "docling");
    ASSERT_EQ(doc.pages.size(), 1u);
    EXPECT_EQ(doc.pages[0].page_num, 1);
    ASSERT_EQ(doc.pages[0].paragraphs.size(), 1u);
    EXPECT_EQ(doc.pages[0].paragraphs[0].text, "Hello world");
    EXPECT_EQ(doc.pages[0].paragraphs[0].section, "Intro");
    EXPECT_EQ(doc.pages[0].paragraphs[0].type, ChunkType::TEXT);
    EXPECT_FLOAT_EQ(doc.pages[0].page_metadata.page_confidence, 0.93f);
    EXPECT_EQ(doc.metadata.doc_title, "Q1 Report");
    EXPECT_EQ(doc.metadata.doc_language, "en");
    EXPECT_EQ(doc.metadata.page_count, 1);
}

TEST_F(ScriptEnv, Docling_BuildArgv_MatchesProtocol) {
    DoclingParser p(CfgFor("/path/docling_bridge.py"));
    ParserOptions opts;
    opts.max_pages = 42;
    opts.subprocess_timeout_ms = 30000;
    opts.language_hint = "zh";
    auto argv = p.BuildArgv("/tmp/x.pdf", opts);
    // python3 script --filepath x --timeout 30 --max-pages 42 --language-hint zh
    // --output-format json
    ASSERT_GE(argv.size(), 12u);
    EXPECT_EQ(argv[0], "python3");
    EXPECT_EQ(argv[1], "/path/docling_bridge.py");
    auto has = [&](const std::string& flag, const std::string& val) {
        for (size_t i = 0; i + 1 < argv.size(); ++i)
            if (argv[i] == flag && argv[i + 1] == val) return true;
        return false;
    };
    EXPECT_TRUE(has("--filepath", "/tmp/x.pdf"));
    EXPECT_TRUE(has("--timeout", "30"));
    EXPECT_TRUE(has("--max-pages", "42"));
    EXPECT_TRUE(has("--language-hint", "zh"));
    EXPECT_TRUE(has("--output-format", "json"));
}

// --- subprocess wrapper error mapping (§4.2) ---

TEST_F(ScriptEnv, Docling_SubprocessFailed_BadPython) {
    DoclingParserConfig cfg = CfgFor("/nonexistent/script.py");
    cfg.python_path = "/nonexistent/python_binary_xyz";
    DoclingParser p(cfg);
    ParsedDoc doc = p.Parse("/tmp/x.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kSubprocessFailed);
    EXPECT_FALSE(doc.retryable);
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_SUBPROCESS_FAILED");
}

TEST_F(ScriptEnv, Docling_SubprocessCrashed_NonZeroExit) {
    std::string script = Write(".py", "import sys\nsys.exit(3)\n");
    DoclingParser p(CfgFor(script));
    ParsedDoc doc = p.Parse("/tmp/x.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kSubprocessCrashed);
    EXPECT_TRUE(doc.retryable);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kTransient);
    EXPECT_EQ(doc.structured_data["exit_code"], 3);
}

TEST_F(ScriptEnv, Docling_CrashCapturesStderrTail) {
    std::string script = Write(".py",
        "import sys\nsys.stderr.write('traceback boom\\n')\nsys.exit(1)\n");
    DoclingParser p(CfgFor(script));
    ParsedDoc doc = p.Parse("/tmp/x.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kSubprocessCrashed);
    EXPECT_NE(doc.structured_data["stderr_tail"].get<std::string>().find("boom"),
              std::string::npos);
}

TEST_F(ScriptEnv, Docling_InvalidJSON) {
    std::string script = Write(".py", "print('not json at all <<<')\n");
    DoclingParser p(CfgFor(script));
    ParsedDoc doc = p.Parse("/tmp/x.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kInvalidOutput);
    EXPECT_TRUE(doc.retryable);  // §5.2: INVALID_OUTPUT is transient
}

TEST_F(ScriptEnv, Docling_Timeout) {
    std::string script = Write(".py", "import time\ntime.sleep(10)\n");
    DoclingParserConfig cfg = CfgFor(script);
    cfg.subprocess_timeout_ms = 300;  // kill well before the 10s sleep
    DoclingParser p(cfg);
    ParserOptions opts;
    opts.subprocess_timeout_ms = 300;
    ParsedDoc doc = p.Parse("/tmp/x.pdf", opts);
    EXPECT_EQ(doc.status, ParserErrorCode::kParseTimeout);
    EXPECT_TRUE(doc.retryable);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kTimeout);
    EXPECT_EQ(doc.retry_after_ms, 1000);
}

TEST_F(ScriptEnv, Docling_OutputTooLarge) {
    // Emit ~200KB but cap output at 4KB → killed, OUTPUT_TOO_LARGE.
    std::string script = Write(".py", "print('x' * 200000)\n");
    DoclingParserConfig cfg = CfgFor(script);
    cfg.max_output_size_bytes = 4096;
    DoclingParser p(cfg);
    ParsedDoc doc = p.Parse("/tmp/x.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kOutputTooLarge);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kQuota);
}

// --- bridge-reported errors are honored (§3.1 error envelope) ---

TEST_F(ScriptEnv, Docling_BridgeReportsPasswordProtected) {
    const char* json = R"JSON({"status": 13, "parser": "docling",
      "error_msg": "Password-protected PDF",
      "structured_data": {"code": "CX_ERR_PASSWORD_PROTECTED",
                          "hint": "provide password"}})JSON";
    DoclingParser p(CfgFor(MockEmitting(json)));
    ParsedDoc doc = p.Parse("/tmp/x.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kPasswordProtected);
    EXPECT_FALSE(doc.retryable);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kPermanent);
    EXPECT_EQ(doc.error_msg, "Password-protected PDF");
    EXPECT_EQ(doc.structured_data["hint"], "provide password");
}

// End-to-end against the *real* docling_bridge.py (not a mock), proving the
// C++↔Python request/response contract. Standalone: docling itself need not be
// installed — the bridge then reports SUBPROCESS_FAILED (CX_ERR_SUBPROCESS_FAILED)
// for a missing dependency, and DoclingParser surfaces it. If docling IS present
// and parses the stub PDF, that's also acceptable (real parse → ok or a doc-level
// error). Real docling parse coverage is D3.5.
TEST_F(ScriptEnv, Docling_RealBridge_ContractHolds) {
    // Locate the repo's docling_bridge.py relative to this test's source tree.
    // tests/unit/ → ../../scripts/docling_bridge.py.
    const char* candidates[] = {
        "scripts/docling_bridge.py",
        "../scripts/docling_bridge.py",
        "../../scripts/docling_bridge.py",
    };
    std::string script;
    for (const char* c : candidates) {
        std::ifstream f(c);
        if (f.good()) { script = c; break; }
    }
    if (script.empty()) {
        GTEST_SKIP() << "docling_bridge.py not found from CWD; skipping real-bridge contract test";
    }

    // A minimal fake PDF (magic bytes); the bridge resolves it as a real path.
    std::string pdf = Write(".pdf", "%PDF-1.4 stub\n");
    DoclingParser p(CfgFor(script));
    ParsedDoc doc = p.Parse(pdf);

    // The bridge always returns a well-formed envelope (exit 0), so the wrapper
    // never sees SUBPROCESS_CRASHED / INVALID_OUTPUT here.
    EXPECT_NE(doc.status, ParserErrorCode::kSubprocessCrashed);
    EXPECT_NE(doc.status, ParserErrorCode::kInvalidOutput);
    if (doc.status == ParserErrorCode::kSubprocessFailed) {
        // docling not installed (the expected standalone path on this machine).
        EXPECT_EQ(doc.structured_data["code"], "CX_ERR_SUBPROCESS_FAILED");
    }
}

// --- ParseBridgeJson unit tests (no subprocess) ---

TEST(ParseBridgeJsonTest, EmptyDocumentIsOkNotError) {
    const char* json = R"JSON({"status": 9, "parser": "docling", "pages": [],
      "metadata": {"filename": "blank.pdf", "page_count": 0},
      "structured_data": {"empty_reason": "all_pages_blank"}})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/tmp/blank.pdf", "docling");
    EXPECT_TRUE(doc.ok());  // §5.1: empty doc => OK, pages=[]
    EXPECT_EQ(doc.status, ParserErrorCode::kEmptyDocument);
    EXPECT_TRUE(doc.pages.empty());
}

TEST(ParseBridgeJsonTest, TableAndImageCaptionTypes) {
    const char* json = R"JSON({"status": 0, "parser": "docling",
      "metadata": {"filename": "t.pdf", "page_count": 1},
      "pages": [{"page_num": 3, "page_text": "tbl",
                 "page_metadata": {"page_num": 3, "parser_used": "docling",
                                   "page_confidence": 0.88, "char_count": 10},
                 "paragraphs": [
                   {"text": "| A | B |", "page": 3, "section": "Data",
                    "type": "TABLE", "confidence": 0.88, "language": "zh"},
                   {"text": "[chart]", "page": 3, "section": "Data",
                    "type": "IMAGE_CAPTION", "confidence": 0.65, "language": "zh",
                    "char_offset": -1, "char_length": -1}]}]})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/tmp/t.pdf", "docling");
    ASSERT_TRUE(doc.ok());
    ASSERT_EQ(doc.pages.size(), 1u);
    ASSERT_EQ(doc.pages[0].paragraphs.size(), 2u);
    EXPECT_EQ(doc.pages[0].paragraphs[0].type, ChunkType::TABLE);
    EXPECT_EQ(doc.pages[0].paragraphs[1].type, ChunkType::IMAGE_CAPTION);
    EXPECT_EQ(doc.pages[0].page_num, 3);
}

// Chinese kept intentionally: verifies CJK page_text + doc_language="zh" parse.
TEST(ParseBridgeJsonTest, ChineseTextAndDocLanguage) {
    const char* json = R"JSON({"status": 0, "parser": "docling",
      "metadata": {"filename": "cn.pdf", "doc_language": "zh", "page_count": 1},
      "pages": [{"page_num": 1, "page_text": "中文文档",
                 "page_metadata": {"page_num": 1, "parser_used": "docling",
                                   "page_confidence": 0.9, "char_count": 4},
                 "paragraphs": [{"text": "中文文档", "page": 1,
                                 "type": "TEXT", "confidence": 0.9,
                                 "language": "zh"}]}]})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/tmp/cn.pdf", "docling");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc.metadata.doc_language, "zh");
    EXPECT_EQ(doc.pages[0].paragraphs[0].text, "\xe4\xb8\xad\xe6\x96\x87\xe6\x96\x87\xe6\xa1\xa3");
}

TEST(ParseBridgeJsonTest, FilenameFallsBackToBasename) {
    // metadata.filename missing → basename of input path.
    const char* json = R"JSON({"status": 0, "parser": "docling",
      "metadata": {"page_count": 0}, "pages": []})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/var/uploads/deck.pptx", "docling");
    EXPECT_EQ(doc.metadata.filename, "deck.pptx");
}

TEST(ParseBridgeJsonTest, MalformedJsonYieldsInvalidOutput) {
    ParsedDoc doc = ParseBridgeJson("{ this is not json", "/tmp/x.pdf", "docling");
    EXPECT_EQ(doc.status, ParserErrorCode::kInvalidOutput);
    EXPECT_FALSE(doc.ok());
}

TEST(ParseBridgeJsonTest, NonObjectTopLevelYieldsInvalidOutput) {
    ParsedDoc doc = ParseBridgeJson("[1,2,3]", "/tmp/x.pdf", "docling");
    EXPECT_EQ(doc.status, ParserErrorCode::kInvalidOutput);
}

TEST(ParseBridgeJsonTest, FailedPagesParsed) {
    const char* json = R"JSON({"status": 0, "parser": "docling",
      "metadata": {"filename": "p.pdf", "page_count": 2},
      "pages": [{"page_num": 1, "page_text": "a",
                 "page_metadata": {"page_num": 1, "char_count": 1},
                 "paragraphs": [{"text": "a", "page": 1, "type": "TEXT"}]}],
      "failed_pages": [3, 17, 42]})JSON";
    ParsedDoc doc = ParseBridgeJson(json, "/tmp/p.pdf", "docling");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc.failed_pages, (std::vector<int>{3, 17, 42}));
}

// --- RunParserSubprocess primitive (used by the wrapper) ---

TEST(RunParserSubprocessTest, CapturesStdoutAndExitCode) {
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "printf hello; exit 0"}, 5000, 1 << 20);
    EXPECT_TRUE(r.launched);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "hello");
}

TEST(RunParserSubprocessTest, LaunchFailureReported) {
    SubprocessResult r = RunParserSubprocess(
        {"/nonexistent/cmd_xyz"}, 5000, 1 << 20);
    EXPECT_FALSE(r.launched);
}

TEST(RunParserSubprocessTest, TimeoutKillsChild) {
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "sleep 10"}, 200, 1 << 20, /*kill_grace_ms=*/200);
    EXPECT_TRUE(r.launched);
    EXPECT_TRUE(r.timed_out);
}

}  // namespace
}  // namespace cortrix::spc
