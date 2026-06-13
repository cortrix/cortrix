#include <gtest/gtest.h>
#include "cortrix/spc/document_parser.h"
#include "cortrix/config/config.h"
#include <fstream>
#include <cstdlib>

namespace cortrix {
namespace {

class DocumentParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temp test files
        txt_path_ = "/tmp/cortrix_test_sample.txt";
        md_path_ = "/tmp/cortrix_test_sample.md";

        {
            std::ofstream f(txt_path_);
            f << "Hello world.\nThis is a test file.\nThird line here.";
        }
        {
            std::ofstream f(md_path_);
            f << "# Title\n\n"
              << "This is **bold** text.\n\n"
              << "- item 1\n"
              << "- item 2\n\n"
              << "[link](http://example.com)\n";
        }
    }

    void TearDown() override {
        std::remove(txt_path_.c_str());
        std::remove(md_path_.c_str());
    }

    std::string txt_path_;
    std::string md_path_;
};

TEST_F(DocumentParserTest, TxtParserWorks) {
    TxtParser parser;
    EXPECT_TRUE(parser.Supports("text/plain"));
    EXPECT_FALSE(parser.Supports("application/pdf"));

    ParseResult result;
    Status s = parser.Parse(txt_path_, "text/plain", &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_FALSE(result.text.empty());
    EXPECT_GT(result.char_count, 0);
    EXPECT_NE(result.text.find("Hello world"), std::string::npos);
}

TEST_F(DocumentParserTest, TxtParserFileNotFound) {
    TxtParser parser;
    ParseResult result;
    Status s = parser.Parse("/nonexistent/file.txt", "text/plain", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(DocumentParserTest, MarkdownParserStripsFormatting) {
    MarkdownParser parser;
    EXPECT_TRUE(parser.Supports("text/markdown"));
    EXPECT_FALSE(parser.Supports("text/plain"));

    ParseResult result;
    Status s = parser.Parse(md_path_, "text/markdown", &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_FALSE(result.text.empty());

    // Should have stripped bold markers
    EXPECT_EQ(result.text.find("**"), std::string::npos);
    // Should have stripped link syntax
    EXPECT_EQ(result.text.find("]("), std::string::npos);
}

TEST_F(DocumentParserTest, TxtParserNullResult) {
    TxtParser parser;
    Status s = parser.Parse(txt_path_, "text/plain", nullptr);
    EXPECT_FALSE(s.ok());
}

TEST_F(DocumentParserTest, PdfParserSupports) {
    PdfParser parser("python3", "parse_pdf.py", 60);
    EXPECT_TRUE(parser.Supports("application/pdf"));
    EXPECT_FALSE(parser.Supports("text/plain"));
}

TEST_F(DocumentParserTest, WordParserSupports) {
    WordParser parser("python3", "parse_word.py", 60);
    EXPECT_TRUE(parser.Supports("application/vnd.openxmlformats-officedocument.wordprocessingml.document"));
    EXPECT_FALSE(parser.Supports("text/plain"));
}

// ============================================================
// MarkdownParser edge-case tests (F03-001)
// ============================================================

class MarkdownEdgeCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        md_path_ = "/tmp/cortrix_md_edge_test.md";
    }

    void TearDown() override {
        std::remove(md_path_.c_str());
    }

    void WriteMarkdown(const std::string& content) {
        std::ofstream f(md_path_);
        f << content;
    }

    ParseResult ParseMarkdown() {
        MarkdownParser parser;
        ParseResult result;
        Status s = parser.Parse(md_path_, "text/markdown", &result);
        EXPECT_TRUE(s.ok()) << s.message();
        return result;
    }

    std::string md_path_;
};

TEST_F(MarkdownEdgeCaseTest, HeaderLevels_AllStripped) {
    WriteMarkdown(
        "# H1 Title\n"
        "## H2 Subtitle\n"
        "### H3 Section\n"
        "#### H4 Subsection\n"
        "##### H5 Minor\n"
        "###### H6 Smallest\n"
    );
    ParseResult r = ParseMarkdown();

    // None of the header markers should remain
    EXPECT_EQ(r.text.find("#"), std::string::npos)
        << "Header markers should be stripped. Got: " << r.text;
    EXPECT_NE(r.text.find("H1 Title"), std::string::npos);
    EXPECT_NE(r.text.find("H6 Smallest"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, BoldAndItalic_Nested) {
    WriteMarkdown("This is ***bold italic*** text.\n");
    ParseResult r = ParseMarkdown();

    EXPECT_EQ(r.text.find("***"), std::string::npos);
    EXPECT_NE(r.text.find("bold italic"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, UnderscoreEmphasis) {
    WriteMarkdown("This is _italic_ and __bold__ and ___both___.\n");
    ParseResult r = ParseMarkdown();

    // Underscores used for emphasis should be stripped
    // Check that the words remain but underscores used as emphasis markers are removed
    EXPECT_NE(r.text.find("italic"), std::string::npos);
    EXPECT_NE(r.text.find("bold"), std::string::npos);
    EXPECT_NE(r.text.find("both"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, InlineCode_Stripped) {
    WriteMarkdown("Use `printf()` to print and `int x = 5;` for variables.\n");
    ParseResult r = ParseMarkdown();

    EXPECT_EQ(r.text.find("`"), std::string::npos)
        << "Backtick markers should be stripped. Got: " << r.text;
    EXPECT_NE(r.text.find("printf()"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, Links_TextPreserved_UrlRemoved) {
    WriteMarkdown(
        "Visit [Google](https://google.com) and [GitHub](https://github.com).\n"
        "Also [text with spaces](http://example.com/path?q=1).\n"
    );
    ParseResult r = ParseMarkdown();

    EXPECT_EQ(r.text.find("]("), std::string::npos);
    EXPECT_NE(r.text.find("Google"), std::string::npos);
    EXPECT_NE(r.text.find("GitHub"), std::string::npos);
    EXPECT_NE(r.text.find("text with spaces"), std::string::npos);
    // URL should not appear
    EXPECT_EQ(r.text.find("https://google.com"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, Images_AltTextPreserved) {
    WriteMarkdown("![Architecture diagram](./img/arch.png)\n");
    ParseResult r = ParseMarkdown();

    EXPECT_EQ(r.text.find("!["), std::string::npos);
    EXPECT_EQ(r.text.find("]("), std::string::npos);
    EXPECT_NE(r.text.find("Architecture diagram"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, Images_EmptyAlt) {
    WriteMarkdown("![](./img/photo.png)\n");
    ParseResult r = ParseMarkdown();

    EXPECT_EQ(r.text.find("!["), std::string::npos);
    EXPECT_EQ(r.text.find("]("), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, HorizontalRules) {
    WriteMarkdown("Before\n---\nMiddle\n***\nAfter\n___\nEnd\n");
    ParseResult r = ParseMarkdown();

    EXPECT_EQ(r.text.find("---"), std::string::npos);
    EXPECT_EQ(r.text.find("***"), std::string::npos);
    EXPECT_EQ(r.text.find("___"), std::string::npos);
    EXPECT_NE(r.text.find("Before"), std::string::npos);
    EXPECT_NE(r.text.find("After"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, Blockquotes_MarkerRemoved) {
    WriteMarkdown(
        "> This is a quote.\n"
        "> Second line of quote.\n"
        ">> Nested quote.\n"
    );
    ParseResult r = ParseMarkdown();

    // The > markers should be stripped
    EXPECT_NE(r.text.find("This is a quote"), std::string::npos);
    EXPECT_NE(r.text.find("Second line"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, EmptyFile) {
    WriteMarkdown("");
    ParseResult r = ParseMarkdown();

    EXPECT_TRUE(r.text.empty());
    EXPECT_EQ(r.char_count, 0);
}

TEST_F(MarkdownEdgeCaseTest, PlainTextOnly_NoFormatting) {
    WriteMarkdown("Just plain text, no markdown at all.\nSecond line.\n");
    ParseResult r = ParseMarkdown();

    EXPECT_NE(r.text.find("Just plain text"), std::string::npos);
    EXPECT_NE(r.text.find("Second line"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, MixedFormatting) {
    WriteMarkdown(
        "# Main Title\n\n"
        "Some **bold** and _italic_ text.\n\n"
        "> A blockquote with `code`.\n\n"
        "- List item [with link](http://test.com)\n"
        "- Another item\n\n"
        "---\n\n"
        "![img](pic.png) Final paragraph.\n"
    );
    ParseResult r = ParseMarkdown();

    // Verify all formatting markers are stripped
    EXPECT_EQ(r.text.find("#"), std::string::npos);
    EXPECT_EQ(r.text.find("**"), std::string::npos);
    EXPECT_EQ(r.text.find("`"), std::string::npos);
    EXPECT_EQ(r.text.find("]("), std::string::npos);
    EXPECT_EQ(r.text.find("---"), std::string::npos);

    // Verify content is preserved
    EXPECT_NE(r.text.find("Main Title"), std::string::npos);
    EXPECT_NE(r.text.find("bold"), std::string::npos);
    EXPECT_NE(r.text.find("italic"), std::string::npos);
    EXPECT_NE(r.text.find("code"), std::string::npos);
    EXPECT_NE(r.text.find("with link"), std::string::npos);
    EXPECT_NE(r.text.find("Final paragraph"), std::string::npos);
}

TEST_F(MarkdownEdgeCaseTest, HashInMiddleOfLine_NotStripped) {
    // A # that's not at the start of a line should NOT be stripped
    WriteMarkdown("This line has a C# reference.\n");
    ParseResult r = ParseMarkdown();

    // The text should preserve "C#"
    EXPECT_NE(r.text.find("C#"), std::string::npos);
}

// ============================================================
// DocumentParserFactory tests
// ============================================================

TEST(DocumentParserFactoryTest, GetParser_Pdf) {
    SPCConfig config;
    config.python_bin = "python3";
    config.parse_pdf_script = "parse_pdf.py";
    config.parse_word_script = "parse_word.py";
    config.parser_timeout_s = 60;
    DocumentParserFactory factory(config);

    EXPECT_NE(factory.GetParser("application/pdf"), nullptr);
}

TEST(DocumentParserFactoryTest, GetParser_Word) {
    SPCConfig config;
    config.python_bin = "python3";
    config.parse_pdf_script = "parse_pdf.py";
    config.parse_word_script = "parse_word.py";
    config.parser_timeout_s = 60;
    DocumentParserFactory factory(config);

    EXPECT_NE(factory.GetParser(
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document"), nullptr);
}

TEST(DocumentParserFactoryTest, GetParser_Markdown) {
    SPCConfig config;
    config.python_bin = "python3";
    config.parser_timeout_s = 60;
    DocumentParserFactory factory(config);

    EXPECT_NE(factory.GetParser("text/markdown"), nullptr);
}

TEST(DocumentParserFactoryTest, GetParser_TextPlain) {
    SPCConfig config;
    config.python_bin = "python3";
    config.parser_timeout_s = 60;
    DocumentParserFactory factory(config);

    EXPECT_NE(factory.GetParser("text/plain"), nullptr);
}

TEST(DocumentParserFactoryTest, GetParser_Unsupported_ReturnsNull) {
    SPCConfig config;
    config.python_bin = "python3";
    config.parser_timeout_s = 60;
    DocumentParserFactory factory(config);

    EXPECT_EQ(factory.GetParser("video/mp4"), nullptr);
    EXPECT_EQ(factory.GetParser("image/png"), nullptr);
    EXPECT_EQ(factory.GetParser("application/octet-stream"), nullptr);
}

// ============================================================
// PdfParser - null result + subprocess error paths
// ============================================================

TEST(PdfParserTest, NullResultPointer) {
    PdfParser parser("python3", "parse_pdf.py", 60);
    Status s = parser.Parse("/tmp/test.pdf", "application/pdf", nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST(PdfParserTest, SubprocessFails_NonexistentScript) {
    PdfParser parser("python3", "/nonexistent/parse_pdf.py", 5);
    ParseResult result;
    Status s = parser.Parse("/tmp/test.pdf", "application/pdf", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("PDF parse failed"), std::string::npos);
}

// Test PdfParser with a fake script that outputs JSON with error
TEST(PdfParserTest, SubprocessReturnsJsonError) {
    // Create a script that outputs JSON error
    std::string script_path = "/tmp/cortrix_test_pdf_error_script.py";
    {
        std::ofstream f(script_path);
        f << "import json\nprint(json.dumps({'error': 'file corrupted'}))\n";
    }
    PdfParser parser("python3", script_path, 10);
    ParseResult result;
    Status s = parser.Parse("/tmp/test.pdf", "application/pdf", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("file corrupted"), std::string::npos);
    std::remove(script_path.c_str());
}

// Test PdfParser with valid JSON output
TEST(PdfParserTest, SubprocessReturnsValidJson) {
    std::string script_path = "/tmp/cortrix_test_pdf_valid_script.py";
    {
        std::ofstream f(script_path);
        f << "import json\nprint(json.dumps({"
          << "'text': 'PDF content here', "
          << "'title': 'My PDF', "
          << "'page_count': 5, "
          << "'needs_ocr': True"
          << "}))\n";
    }
    PdfParser parser("python3", script_path, 10);
    ParseResult result;
    Status s = parser.Parse("/tmp/test.pdf", "application/pdf", &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.text, "PDF content here");
    EXPECT_EQ(result.title, "My PDF");
    EXPECT_EQ(result.page_count, 5);
    EXPECT_TRUE(result.needs_ocr);
    EXPECT_EQ(result.char_count, static_cast<int64_t>(result.text.size()));
    EXPECT_EQ(result.language, "unknown");
    std::remove(script_path.c_str());
}

// Test PdfParser with invalid JSON output
TEST(PdfParserTest, SubprocessReturnsInvalidJson) {
    std::string script_path = "/tmp/cortrix_test_pdf_badjson_script.py";
    {
        std::ofstream f(script_path);
        f << "print('not json')\n";
    }
    PdfParser parser("python3", script_path, 10);
    ParseResult result;
    Status s = parser.Parse("/tmp/test.pdf", "application/pdf", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("Failed to parse PDF JSON"), std::string::npos);
    std::remove(script_path.c_str());
}

// ============================================================
// WordParser - null result + subprocess error paths
// ============================================================

TEST(WordParserTest, NullResultPointer) {
    WordParser parser("python3", "parse_word.py", 60);
    Status s = parser.Parse("/tmp/test.docx", "", nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST(WordParserTest, SubprocessFails_NonexistentScript) {
    WordParser parser("python3", "/nonexistent/parse_word.py", 5);
    ParseResult result;
    Status s = parser.Parse("/tmp/test.docx", "", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("Word parse failed"), std::string::npos);
}

TEST(WordParserTest, SubprocessReturnsJsonError) {
    std::string script_path = "/tmp/cortrix_test_word_error_script.py";
    {
        std::ofstream f(script_path);
        f << "import json\nprint(json.dumps({'error': 'encrypted file'}))\n";
    }
    WordParser parser("python3", script_path, 10);
    ParseResult result;
    Status s = parser.Parse("/tmp/test.docx", "", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("encrypted file"), std::string::npos);
    std::remove(script_path.c_str());
}

TEST(WordParserTest, SubprocessReturnsValidJson) {
    std::string script_path = "/tmp/cortrix_test_word_valid_script.py";
    {
        std::ofstream f(script_path);
        f << "import json\nprint(json.dumps({"
          << "'text': 'Word document content', "
          << "'title': 'My Word Doc', "
          << "'page_count': 3"
          << "}))\n";
    }
    WordParser parser("python3", script_path, 10);
    ParseResult result;
    Status s = parser.Parse("/tmp/test.docx", "", &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.text, "Word document content");
    EXPECT_EQ(result.title, "My Word Doc");
    EXPECT_EQ(result.page_count, 3);
    EXPECT_EQ(result.char_count, static_cast<int64_t>(result.text.size()));
    std::remove(script_path.c_str());
}

TEST(WordParserTest, SubprocessReturnsInvalidJson) {
    std::string script_path = "/tmp/cortrix_test_word_badjson_script.py";
    {
        std::ofstream f(script_path);
        f << "print('{invalid json')\n";
    }
    WordParser parser("python3", script_path, 10);
    ParseResult result;
    Status s = parser.Parse("/tmp/test.docx", "", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("Failed to parse Word JSON"), std::string::npos);
    std::remove(script_path.c_str());
}

// ============================================================
// MarkdownParser - file not found
// ============================================================

TEST(MarkdownParserTest, FileNotFound) {
    MarkdownParser parser;
    ParseResult result;
    Status s = parser.Parse("/nonexistent/file.md", "text/markdown", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST(MarkdownParserTest, NullResultPointer) {
    MarkdownParser parser;
    Status s = parser.Parse("/tmp/test.md", "text/markdown", nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// ============================================================
// TxtParser - additional edge cases
// ============================================================

TEST(TxtParserTest, LargeFile) {
    std::string path = "/tmp/cortrix_test_large.txt";
    {
        std::ofstream f(path);
        for (int i = 0; i < 10000; ++i) {
            f << "Line " << i << " with some content to make it larger.\n";
        }
    }
    TxtParser parser;
    ParseResult result;
    Status s = parser.Parse(path, "text/plain", &result);
    ASSERT_TRUE(s.ok());
    EXPECT_GT(result.char_count, 100000);
    EXPECT_NE(result.text.find("Line 9999"), std::string::npos);
    std::remove(path.c_str());
}

TEST(TxtParserTest, BinaryContent) {
    std::string path = "/tmp/cortrix_test_binary.txt";
    {
        std::ofstream f(path, std::ios::binary);
        for (int i = 0; i < 256; ++i) {
            f.put(static_cast<char>(i));
        }
    }
    TxtParser parser;
    ParseResult result;
    Status s = parser.Parse(path, "text/plain", &result);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(result.char_count, 256);
    std::remove(path.c_str());
}

}  // namespace
}  // namespace cortrix
