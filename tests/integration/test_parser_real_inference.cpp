#include <gtest/gtest.h>
#include "cortrix/spc/docling_parser.h"
#include "cortrix/spc/paddleocr_parser.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace cortrix::spc {
namespace {

namespace fs = std::filesystem;

// Real-parser inference (integration): drives the actual docling / paddleocr
// libraries through the production bridges against committed fixtures.
// SKIPPED unless CORTRIX_PARSER_PYTHON points at a python with the
// requirements-parser.txt deps installed (docling needs its layout models
// downloaded on first run — slow; this test assumes a warmed cache).

static std::string FindRepoFile(const std::string& rel) {
    std::vector<std::string> roots = {".", "..", "../..", "../../.."};
    const char* src_dir = std::getenv("CORTRIX_SOURCE_DIR");
    if (src_dir) roots.insert(roots.begin(), src_dir);
    for (const auto& r : roots) {
        std::string p = r + "/" + rel;
        if (fs::exists(p)) return p;
    }
    return "";
}

class ParserRealInferenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* py = std::getenv("CORTRIX_PARSER_PYTHON");
        if (!py || !fs::exists(py)) {
            GTEST_SKIP() << "CORTRIX_PARSER_PYTHON not set (python with "
                            "requirements-parser.txt deps); skipping real parser test";
        }
        python_ = py;
    }

    std::string python_;
};

TEST_F(ParserRealInferenceTest, DoclingParsesRealPdf) {
    std::string script = FindRepoFile("scripts/docling_bridge.py");
    std::string pdf = FindRepoFile("tests/fixtures/parser/sample_text.pdf");
    ASSERT_FALSE(script.empty());
    ASSERT_FALSE(pdf.empty());

    DoclingParserConfig cfg;
    cfg.python_path = python_;
    cfg.script_path = script;
    cfg.subprocess_timeout_ms = 300000;  // cold model load is slow
    DoclingParser parser(cfg);

    ParserOptions opts;
    opts.language_hint = "en";
    opts.subprocess_timeout_ms = 300000;
    ParsedDoc doc = parser.Parse(pdf, opts);

    ASSERT_EQ(doc.status, ParserErrorCode::kOk) << doc.error_msg;
    EXPECT_EQ(doc.parser_name, "docling");
    ASSERT_EQ(doc.pages.size(), 1u);
    std::cout << "[DOCLING] page_text: " << doc.pages[0].page_text << std::endl;
    EXPECT_NE(doc.pages[0].page_text.find("Cortrix semantic storage engine"),
              std::string::npos);
    EXPECT_NE(doc.pages[0].page_text.find("1234.56"), std::string::npos);
}

TEST_F(ParserRealInferenceTest, PaddleOcrParsesRealImage) {
    std::string script = FindRepoFile("scripts/paddleocr_bridge.py");
    std::string png = FindRepoFile("tests/fixtures/parser/sample_text.png");
    ASSERT_FALSE(script.empty());
    ASSERT_FALSE(png.empty());

    PaddleOCRParserConfig cfg;
    cfg.python_path = python_;
    cfg.script_path = script;
    cfg.subprocess_timeout_ms = 300000;
    cfg.lang = "en";
    PaddleOCRParser parser(cfg);

    ParserOptions opts;
    opts.language_hint = "en";
    opts.subprocess_timeout_ms = 300000;
    ParsedDoc doc = parser.Parse(png, opts);

    ASSERT_EQ(doc.status, ParserErrorCode::kOk) << doc.error_msg;
    EXPECT_EQ(doc.parser_name, "paddleocr");
    ASSERT_EQ(doc.pages.size(), 1u);
    std::cout << "[PADDLEOCR] page_text: " << doc.pages[0].page_text << std::endl;
    EXPECT_NE(doc.pages[0].page_text.find("Cortrix semantic storage engine"),
              std::string::npos);
    EXPECT_NE(doc.pages[0].page_text.find("1234.56"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::spc
