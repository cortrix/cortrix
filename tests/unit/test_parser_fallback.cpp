#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cortrix/spc/docling_parser.h"
#include "cortrix/spc/paddleocr_parser.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_factory.h"
#include "parser_script_fixture.h"

// Parser S3 coverage: the Docling→PaddleOCR fallback chain, wired end-to-end with
// the *real* DoclingParser + PaddleOCRParser (each pointed at a mock bridge) in
// the factory. Exercises §4.1 step 4-5 orchestration as actually composed —
// not stubs of IDocumentParser, but the real wrapper classes over mock scripts.
namespace cortrix::spc {
namespace {

using test::ScriptFixture;

// Docling payload with one low-confidence page (trips the fallback threshold).
const char* kLowConfDocling = R"JSON({
  "status": 0, "parser": "docling",
  "metadata": {"filename": "scan.pdf", "page_count": 1},
  "pages": [{"page_num": 1, "page_text": "garble",
             "page_metadata": {"page_num": 1, "parser_used": "docling",
                               "page_confidence": 0.1, "is_scan_page": false,
                               "char_count": 6},
             "paragraphs": [{"text": "garble", "page": 1, "type": "TEXT",
                             "confidence": 0.1, "language": ""}]}]})JSON";

// Docling payload that returns zero pages (empty doc).
const char* kEmptyDocling = R"JSON({
  "status": 0, "parser": "docling",
  "metadata": {"filename": "scan.pdf", "page_count": 0}, "pages": []})JSON";

// Healthy Docling payload (high confidence → no fallback).
const char* kGoodDocling = R"JSON({
  "status": 0, "parser": "docling",
  "metadata": {"filename": "doc.pdf", "page_count": 1},
  "pages": [{"page_num": 1, "page_text": "clean text",
             "page_metadata": {"page_num": 1, "parser_used": "docling",
                               "page_confidence": 0.95, "is_scan_page": false,
                               "char_count": 10},
             "paragraphs": [{"text": "clean text", "page": 1, "type": "TEXT",
                             "confidence": 0.95, "language": "en"}]}]})JSON";

// PaddleOCR success payload.
const char* kGoodOcr = R"JSON({
  "status": 0, "parser": "paddleocr",
  "metadata": {"filename": "scan.pdf", "page_count": 1},
  "pages": [{"page_num": 1, "page_text": "recovered",
             "page_metadata": {"page_num": 1, "parser_used": "paddleocr",
                               "page_confidence": 0.85, "is_scan_page": true,
                               "char_count": 9},
             "paragraphs": [{"text": "recovered", "page": 1, "type": "TEXT",
                             "confidence": 0.85, "language": "zh"}]}]})JSON";

class FallbackChainTest : public ScriptFixture {
protected:
    // Build a factory whose primary = real DoclingParser over `docling_json`,
    // fallback = real PaddleOCRParser over `ocr_json` (both mock bridges).
    DocumentParserFactory MakeFactory(const std::string& docling_json,
                                      const std::string& ocr_json) {
        ParserFactoryConfig cfg;
        DocumentParserFactory factory(cfg);
        DoclingParserConfig dc;
        dc.python_path = "python3";
        dc.script_path = MockEmitting(docling_json);
        factory.SetPrimaryParser(std::make_unique<DoclingParser>(dc));
        PaddleOCRParserConfig pc;
        pc.python_path = "python3";
        pc.script_path = MockEmitting(ocr_json);
        factory.SetFallbackParser(std::make_unique<PaddleOCRParser>(pc));
        return factory;
    }

    // A real on-disk PDF (factory pre-check stats it).
    std::string Pdf() { return Write(".pdf", "%PDF-1.4 stub\n"); }
};

TEST_F(FallbackChainTest, DoclingEmpty_FallsBackToOCR) {
    DocumentParserFactory f = MakeFactory(kEmptyDocling, kGoodOcr);
    ParsedDoc doc = f.ParseDocument(Pdf());
    ASSERT_TRUE(doc.ok());
    // Docling produced nothing → pure OCR result.
    EXPECT_EQ(doc.parser_name, "paddleocr");
    ASSERT_EQ(doc.pages.size(), 1u);
    EXPECT_EQ(doc.pages[0].paragraphs[0].text, "recovered");
}

TEST_F(FallbackChainTest, DoclingLowConfidence_FallsBackToOCR) {
    DocumentParserFactory f = MakeFactory(kLowConfDocling, kGoodOcr);
    ParsedDoc doc = f.ParseDocument(Pdf());
    ASSERT_TRUE(doc.ok());
    // Both produced pages → mixed parser name.
    EXPECT_EQ(doc.parser_name, "docling+paddleocr");
}

TEST_F(FallbackChainTest, DoclingHealthy_NoFallback) {
    DocumentParserFactory f = MakeFactory(kGoodDocling, kGoodOcr);
    ParsedDoc doc = f.ParseDocument(Pdf());
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc.parser_name, "docling");
    EXPECT_EQ(doc.pages[0].paragraphs[0].text, "clean text");
}

TEST_F(FallbackChainTest, BothFail_AllParsersFailed) {
    // Docling crashes (exit 1 → SUBPROCESS_CRASHED), OCR also crashes
    // (exit 1 → OCR_FAILED). NeedsFallback true on the (non-empty-error) result.
    ParserFactoryConfig cfg;
    DocumentParserFactory factory(cfg);
    DoclingParserConfig dc;
    dc.script_path = Write(".py", "import sys\nsys.exit(1)\n");
    factory.SetPrimaryParser(std::make_unique<DoclingParser>(dc));
    PaddleOCRParserConfig pc;
    pc.script_path = Write(".py", "import sys\nsys.exit(1)\n");
    factory.SetFallbackParser(std::make_unique<PaddleOCRParser>(pc));

    ParsedDoc doc = factory.ParseDocument(Pdf());
    EXPECT_EQ(doc.status, ParserErrorCode::kAllParsersFailed);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kPermanent);
    ASSERT_TRUE(doc.structured_data["attempted"].is_array());
    EXPECT_EQ(doc.structured_data["attempted"].size(), 2u);
}

TEST_F(FallbackChainTest, ImageRoutesDirectlyToOCR) {
    // A PNG never touches Docling — the factory routes images straight to OCR.
    ParserFactoryConfig cfg;
    DocumentParserFactory factory(cfg);
    DoclingParserConfig dc;
    dc.script_path = Write(".py", "import sys\nsys.exit(99)\n");  // would crash
    factory.SetPrimaryParser(std::make_unique<DoclingParser>(dc));
    PaddleOCRParserConfig pc;
    pc.script_path = MockEmitting(kGoodOcr);
    factory.SetFallbackParser(std::make_unique<PaddleOCRParser>(pc));

    std::string png = Write(".png", "\x89PNG fake");
    ParsedDoc doc = factory.ParseDocument(png);
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc.parser_name, "paddleocr");
}

TEST_F(FallbackChainTest, FallbackDisabled_KeepsLowConfDocling) {
    DocumentParserFactory f = MakeFactory(kLowConfDocling, kGoodOcr);
    ParserOptions opts;
    opts.enable_ocr_fallback = false;
    ParsedDoc doc = f.ParseDocument(Pdf(), opts);
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc.parser_name, "docling");  // no OCR despite low confidence
}

}  // namespace
}  // namespace cortrix::spc
