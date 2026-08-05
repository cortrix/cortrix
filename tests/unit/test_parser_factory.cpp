#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_factory.h"
#include "parser_stub.h"

// Parser S1 coverage: DocumentParserFactory pre-check / extension→MIME routing /
// fallback orchestration / custom parser registration. Standalone — the primary
// + fallback parsers are StubParsers (no Python / Docling / OCR).
namespace cortrix::spc {
namespace {

using test::MakeOnePageDoc;
using test::StubParser;

// RAII temp file so the factory's stat() pre-check sees a real file of a known
// size and extension.
class TempFile {
public:
    TempFile(const std::string& suffix, size_t bytes) {
        // mkstemp creates+opens a unique file securely; we then rename to add the
        // extension the factory routes on (mkstemp itself can't take a suffix).
        char tmpl[] = "/tmp/cortrix_f06_XXXXXX";
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

// A factory with Docling-like primary + PaddleOCR-like fallback stubs wired in,
// matching the S2/S3 injection points.
struct Fixture {
    ParserFactoryConfig config;
    DocumentParserFactory factory{config};
    StubParser* primary;
    StubParser* fallback;

    Fixture() {
        auto p = std::make_unique<StubParser>(
            "docling", std::vector<std::string>{"pdf", "docx", "pptx", "xlsx",
                                                "html", "md", "txt"});
        auto f = std::make_unique<StubParser>(
            "paddleocr", std::vector<std::string>{"pdf", "png", "jpg", "tiff"});
        primary = p.get();
        fallback = f.get();
        primary->SetResult(MakeOnePageDoc("docling", 0.9f));
        fallback->SetResult(MakeOnePageDoc("paddleocr", 0.8f));
        factory.SetPrimaryParser(std::move(p));
        factory.SetFallbackParser(std::move(f));
    }
};

// --- pre-check ---

TEST(ParserFactoryTest, Factory_FileNotFound) {
    Fixture fx;
    ParsedDoc doc = fx.factory.ParseDocument("/nonexistent/path/to/file.pdf");
    EXPECT_EQ(doc.status, ParserErrorCode::kFileNotFound);
    EXPECT_FALSE(doc.ok());
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kPermanent);
    ASSERT_TRUE(doc.structured_data.is_object());
    EXPECT_EQ(doc.structured_data["code"], "CX_ERR_FILE_NOT_FOUND");
    // Primary never ran.
    EXPECT_EQ(fx.primary->call_count(), 0);
}

TEST(ParserFactoryTest, Factory_FileTooLarge) {
    Fixture fx;
    TempFile big(".pdf", 2 * 1024 * 1024);  // 2MB file
    ParserOptions opts;
    opts.max_file_size_mb = 1;              // limit 1MB → exceeds
    ParsedDoc doc = fx.factory.ParseDocument(big.path(), opts);
    EXPECT_EQ(doc.status, ParserErrorCode::kFileTooLarge);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kQuota);
    EXPECT_EQ(doc.structured_data["limit_mb"], 1);
    EXPECT_EQ(doc.structured_data["tier"], "ce");
    EXPECT_GE(doc.structured_data["actual_size_mb"].get<int>(), 1);
    EXPECT_EQ(fx.primary->call_count(), 0);
}

TEST(ParserFactoryTest, Factory_UnsupportedFormat) {
    Fixture fx;
    TempFile exe(".exe", 16);
    ParsedDoc doc = fx.factory.ParseDocument(exe.path());
    EXPECT_EQ(doc.status, ParserErrorCode::kUnsupportedFormat);
    EXPECT_EQ(doc.structured_data["extension"], "exe");
    EXPECT_TRUE(doc.structured_data["supported_formats"].is_array());
    EXPECT_EQ(fx.primary->call_count(), 0);
}

// --- routing / success ---

TEST(ParserFactoryTest, Factory_ParserNameInResult) {
    Fixture fx;
    TempFile pdf(".pdf", 1024);
    ParsedDoc doc = fx.factory.ParseDocument(pdf.path());
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(doc.parser_name, "docling");
    EXPECT_EQ(fx.primary->call_count(), 1);
    EXPECT_EQ(fx.fallback->call_count(), 0);  // high confidence → no fallback
}

TEST(ParserFactoryTest, Factory_ImageRoutesToOcr) {
    Fixture fx;
    TempFile png(".png", 1024);
    ParsedDoc doc = fx.factory.ParseDocument(png.path());
    EXPECT_TRUE(doc.ok());
    // Pure image goes straight to the OCR parser; the "primary" Docling is skipped.
    EXPECT_EQ(doc.parser_name, "paddleocr");
    EXPECT_EQ(fx.primary->call_count(), 0);
    EXPECT_EQ(fx.fallback->call_count(), 1);
}

TEST(ParserFactoryTest, Factory_OptionsForwardedToParser) {
    Fixture fx;
    TempFile pdf(".pdf", 1024);
    int seen_max_pages = -1;
    fx.primary->SetOnParse([&](const std::string&, const ParserOptions& o) {
        seen_max_pages = o.max_pages;
    });
    ParserOptions opts;
    opts.max_pages = 42;
    fx.factory.ParseDocument(pdf.path(), opts);
    EXPECT_EQ(seen_max_pages, 42);
}

// --- fallback orchestration ---

TEST(ParserFactoryTest, Factory_FallbackTriggered_EmptyPages) {
    Fixture fx;
    TempFile pdf(".pdf", 1024);
    // Docling returns 0 pages (status OK) → factory falls back to OCR (Q2).
    ParsedDoc empty;
    empty.status = ParserErrorCode::kOk;
    empty.parser_name = "docling";
    fx.primary->SetResult(empty);
    ParsedDoc doc = fx.factory.ParseDocument(pdf.path());
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(fx.fallback->call_count(), 1);
    // Primary produced nothing → pure OCR result name.
    EXPECT_EQ(doc.parser_name, "paddleocr");
}

TEST(ParserFactoryTest, Factory_FallbackTriggered_LowConfidence) {
    Fixture fx;
    TempFile pdf(".pdf", 1024);
    fx.primary->SetResult(MakeOnePageDoc("docling", 0.1f));   // below 0.3 threshold
    fx.fallback->SetResult(MakeOnePageDoc("paddleocr", 0.85f));
    ParsedDoc doc = fx.factory.ParseDocument(pdf.path());
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(fx.fallback->call_count(), 1);
    // Both produced pages → mixed parser name.
    EXPECT_EQ(doc.parser_name, "docling+paddleocr");
}

TEST(ParserFactoryTest, Factory_FallbackDisabled) {
    Fixture fx;
    TempFile pdf(".pdf", 1024);
    fx.primary->SetResult(MakeOnePageDoc("docling", 0.1f));   // low confidence
    ParserOptions opts;
    opts.enable_ocr_fallback = false;                         // but fallback off
    ParsedDoc doc = fx.factory.ParseDocument(pdf.path(), opts);
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(fx.fallback->call_count(), 0);
    EXPECT_EQ(doc.parser_name, "docling");                    // keep low-conf result
}

TEST(ParserFactoryTest, Factory_FallbackBothFail) {
    Fixture fx;
    TempFile pdf(".pdf", 1024);
    // Primary errors, OCR also errors → ALL_PARSERS_FAILED.
    ParsedDoc primary_err;
    primary_err.status = ParserErrorCode::kSubprocessCrashed;
    fx.primary->SetResult(primary_err);
    ParsedDoc ocr_err;
    ocr_err.status = ParserErrorCode::kOcrFailed;
    fx.fallback->SetResult(ocr_err);
    ParsedDoc doc = fx.factory.ParseDocument(pdf.path());
    EXPECT_EQ(doc.status, ParserErrorCode::kAllParsersFailed);
    EXPECT_EQ(doc.category, agent_friendly::ErrorCategory::kPermanent);
    ASSERT_TRUE(doc.structured_data["attempted"].is_array());
    EXPECT_EQ(doc.structured_data["attempted"].size(), 2u);
}

TEST(ParserFactoryTest, Factory_PasswordProtected_NoFallback) {
    Fixture fx;
    TempFile pdf(".pdf", 1024);
    // §4.1 step 5: password-protected does not fall back to OCR.
    ParsedDoc pwd;
    pwd.status = ParserErrorCode::kPasswordProtected;
    fx.primary->SetResult(pwd);
    ParsedDoc doc = fx.factory.ParseDocument(pdf.path());
    EXPECT_EQ(doc.status, ParserErrorCode::kPasswordProtected);
    EXPECT_EQ(fx.fallback->call_count(), 0);
}

// --- NeedsFallback unit logic (public for direct testing) ---

TEST(ParserFactoryTest, NeedsFallback_Logic) {
    ParserFactoryConfig config;
    DocumentParserFactory factory(config);

    EXPECT_TRUE(factory.NeedsFallback(ParsedDoc{}, 0.3f));         // empty pages
    EXPECT_FALSE(factory.NeedsFallback(MakeOnePageDoc("d", 0.9f), 0.3f));  // healthy
    EXPECT_TRUE(factory.NeedsFallback(MakeOnePageDoc("d", 0.1f), 0.3f));   // low conf

    // Hard file errors never fall back regardless of pages.
    ParsedDoc pwd = MakeOnePageDoc("d", 0.9f);
    pwd.status = ParserErrorCode::kPasswordProtected;
    EXPECT_FALSE(factory.NeedsFallback(pwd, 0.3f));

    // High failed-page ratio triggers fallback.
    ParsedDoc partial = MakeOnePageDoc("d", 0.9f);
    partial.failed_pages = {2, 3};  // 1 ok page, 2 failed → ratio 0.66 > 0.5
    EXPECT_TRUE(factory.NeedsFallback(partial, 0.3f));
}

// --- registration + MIME mapping ---

TEST(ParserFactoryTest, Factory_RegisterCustomParser) {
    Fixture fx;
    EXPECT_EQ(fx.factory.ListParsers(),
              (std::vector<std::string>{"docling", "paddleocr"}));
    fx.factory.RegisterParser(
        "vlm", std::make_unique<StubParser>("vlm", std::vector<std::string>{"pdf"}));
    auto names = fx.factory.ListParsers();
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names.back(), "vlm");
}

TEST(ParserFactoryTest, Factory_MimeTypeMapping) {
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("pdf"), "application/pdf");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("PDF"), "application/pdf");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("docx"),
              "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("pptx"),
              "application/vnd.openxmlformats-officedocument.presentationml.presentation");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("xlsx"),
              "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("html"), "text/html");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("md"), "text/markdown");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("txt"), "text/plain");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("png"), "image/png");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("exe"), "");  // unknown
}

TEST(ParserFactoryTest, ExtractExtension_Cases) {
    EXPECT_EQ(DocumentParserFactory::ExtractExtension("/a/b/c.PDF"), "pdf");
    EXPECT_EQ(DocumentParserFactory::ExtractExtension("report.tar.gz"), "gz");
    EXPECT_EQ(DocumentParserFactory::ExtractExtension("noext"), "");
    EXPECT_EQ(DocumentParserFactory::ExtractExtension("/path/.bashrc"), "");
    EXPECT_EQ(DocumentParserFactory::ExtractExtension("trailing."), "");
}

// ============================================================
// Branch coverage: remaining MIME arms / NeedsFallback arms / null-parser routing
// ============================================================

// Every remaining ExtensionToMimeType arm (htm / markdown / jpeg / tif / bmp /
// jpg) so each `if` in the chain is exercised at least once.
TEST(ParserFactoryTest, MimeTypeMapping_RemainingArms) {
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("htm"), "text/html");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("markdown"), "text/markdown");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("jpg"), "image/jpeg");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("jpeg"), "image/jpeg");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("tif"), "image/tiff");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("tiff"), "image/tiff");
    EXPECT_EQ(DocumentParserFactory::ExtensionToMimeType("bmp"), "image/bmp");
}

// NeedsFallback: a corrupted-file hard error never falls back (the
// kCorruptedFile arm of the early-return guard, distinct from the password arm
// covered above).
TEST(ParserFactoryTest, NeedsFallback_CorruptedFileNeverFallsBack) {
    ParserFactoryConfig config;
    DocumentParserFactory factory(config);
    ParsedDoc corrupt = MakeOnePageDoc("d", 0.05f);  // low conf would normally fall back
    corrupt.status = ParserErrorCode::kCorruptedFile;
    EXPECT_FALSE(factory.NeedsFallback(corrupt, 0.3f));
}

// NeedsFallback: healthy pages, healthy failed-ratio, healthy confidence → the
// final "return false" (no fallback) — the all-guards-false path.
TEST(ParserFactoryTest, NeedsFallback_AllHealthyReturnsFalse) {
    ParserFactoryConfig config;
    DocumentParserFactory factory(config);
    ParsedDoc good = MakeOnePageDoc("d", 0.95f);
    EXPECT_FALSE(factory.NeedsFallback(good, 0.3f));
}

// SelectPrimary returns null when the route has no registered parser. An image
// route with no fallback parser set → ParseDocument hits the null-primary branch
// and reports ALL_PARSERS_FAILED with an empty attempted[] array.
TEST(ParserFactoryTest, Factory_NoParserForRoute_AllParsersFailed) {
    ParserFactoryConfig config;
    DocumentParserFactory factory(config);  // no primary, no fallback
    TempFile png(".png", 64);               // routes to (absent) fallback
    ParsedDoc doc = factory.ParseDocument(png.path());
    EXPECT_EQ(doc.status, ParserErrorCode::kAllParsersFailed);
    ASSERT_TRUE(doc.structured_data["attempted"].is_array());
    EXPECT_TRUE(doc.structured_data["attempted"].empty());
}

// Low-confidence primary result + OCR fallback that returns EMPTY (ok but no
// pages) → the factory keeps the primary's low-confidence result (the "OCR added
// nothing → keep primary" branch, distinct from both-fail and mixed-result).
TEST(ParserFactoryTest, Factory_FallbackEmptyKeepsPrimary) {
    Fixture fx;
    TempFile pdf(".pdf", 1024);
    fx.primary->SetResult(MakeOnePageDoc("docling", 0.1f));  // low conf → triggers fallback
    ParsedDoc ocr_empty;
    ocr_empty.status = ParserErrorCode::kOk;  // ok but zero pages
    fx.fallback->SetResult(ocr_empty);
    ParsedDoc doc = fx.factory.ParseDocument(pdf.path());
    EXPECT_EQ(fx.fallback->call_count(), 1);
    // OCR produced nothing usable → primary's (low-conf but non-empty) result stands.
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(doc.parser_name, "docling");
}

// enable_paddleocr=false in the factory config disables the fallback even when the
// primary trips the low-confidence threshold (the config_.enable_paddleocr arm of
// fallback_allowed, distinct from the per-request enable_ocr_fallback off case).
TEST(ParserFactoryTest, Factory_PaddleOcrDisabledInConfig_NoFallback) {
    ParserFactoryConfig config;
    config.enable_paddleocr = false;
    DocumentParserFactory factory(config);
    auto p = std::make_unique<StubParser>(
        "docling", std::vector<std::string>{"pdf"});
    auto f = std::make_unique<StubParser>(
        "paddleocr", std::vector<std::string>{"pdf"});
    StubParser* primary = p.get();
    StubParser* fallback = f.get();
    primary->SetResult(MakeOnePageDoc("docling", 0.1f));  // low conf
    factory.SetPrimaryParser(std::move(p));
    factory.SetFallbackParser(std::move(f));

    TempFile pdf(".pdf", 1024);
    ParsedDoc doc = factory.ParseDocument(pdf.path());
    EXPECT_EQ(fallback->call_count(), 0);  // config gate kept OCR off
    EXPECT_EQ(doc.parser_name, "docling");
}

// ============================================================
// Additional decision-branch coverage: each operand of the IsImageExt / MimeType
// short-circuit chains, RegisterParser's dup arm, ListParsers' null arms, and the
// max_file_size_mb<=0 (unlimited) arm. These target the second/third operands of
// `||` chains and the FALSE side of guards that the existing happy-path tests
// short-circuit past.
// ============================================================

// SelectPrimary -> IsImageExt: route each non-png image extension so every
// operand of the 6-way `||` (jpg/jpeg/tiff/tif/bmp) is the one that decides true.
// A factory with only a fallback (OCR) parser: an image ext routes to it.
TEST(ParserFactoryTest, IsImageExt_EveryImageOperandRoutesToOcr) {
    for (const char* ext : {"jpg", "jpeg", "tiff", "tif", "bmp"}) {
        ParserFactoryConfig config;
        DocumentParserFactory factory(config);
        auto f = std::make_unique<StubParser>(
            "paddleocr", std::vector<std::string>{"pdf", "png", "jpg", "jpeg",
                                                  "tiff", "tif", "bmp"});
        StubParser* fallback = f.get();
        fallback->SetResult(MakeOnePageDoc("paddleocr", 0.9f));
        factory.SetFallbackParser(std::move(f));
        // Also set a primary so the route choice (image->fallback) is observable.
        auto p = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
        StubParser* primary = p.get();
        factory.SetPrimaryParser(std::move(p));

        TempFile img(std::string(".") + ext, 256);
        ParsedDoc doc = factory.ParseDocument(img.path());
        EXPECT_EQ(primary->call_count(), 0) << ext << ": image must skip primary";
        EXPECT_EQ(fallback->call_count(), 1) << ext << ": image must hit OCR";
    }
}

// A non-image, non-pdf extension that still maps to a MIME type ("docx") routes to
// the primary - exercises IsImageExt returning false through ALL six operands.
TEST(ParserFactoryTest, IsImageExt_AllFalseRoutesToPrimary) {
    Fixture fx;
    TempFile docx(".docx", 256);
    ParsedDoc doc = fx.factory.ParseDocument(docx.path());
    EXPECT_EQ(fx.primary->call_count(), 1);
    EXPECT_EQ(fx.fallback->call_count(), 0);
}

// RegisterParser called twice with the SAME name -> the second call takes the
// `find != end` (already-present) FALSE-of-the-guard arm: custom_order_ is NOT
// appended again, but the parser pointer is replaced.
TEST(ParserFactoryTest, RegisterParser_DuplicateNameNoReorder) {
    Fixture fx;
    fx.factory.RegisterParser(
        "vlm", std::make_unique<StubParser>("vlm", std::vector<std::string>{"pdf"}));
    auto after_first = fx.factory.ListParsers();
    fx.factory.RegisterParser(  // same name again
        "vlm", std::make_unique<StubParser>("vlm", std::vector<std::string>{"png"}));
    auto after_second = fx.factory.ListParsers();
    EXPECT_EQ(after_first, after_second);  // no duplicate "vlm" entry
    int vlm_count = 0;
    for (const auto& n : after_second) if (n == "vlm") ++vlm_count;
    EXPECT_EQ(vlm_count, 1);
}

// ListParsers on a factory with NO primary and NO fallback -> both `if (primary_)`
// and `if (fallback_)` take the FALSE arm; only custom parsers (if any) appear.
TEST(ParserFactoryTest, ListParsers_NoPrimaryNoFallback) {
    ParserFactoryConfig config;
    DocumentParserFactory factory(config);  // nothing set
    EXPECT_TRUE(factory.ListParsers().empty());
    factory.RegisterParser(
        "only", std::make_unique<StubParser>("only", std::vector<std::string>{"pdf"}));
    auto names = factory.ListParsers();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "only");
}

// max_file_size_mb == 0 -> the `opts.max_file_size_mb > 0` guard is FALSE, so the
// size check is skipped entirely (unlimited). A large file parses fine.
TEST(ParserFactoryTest, MaxFileSizeZero_SkipsSizeCheck) {
    Fixture fx;
    TempFile big(".pdf", 4 * 1024 * 1024);  // 4MB
    ParserOptions opts;
    opts.max_file_size_mb = 0;  // unlimited, no FileTooLarge
    ParsedDoc doc = fx.factory.ParseDocument(big.path(), opts);
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(fx.primary->call_count(), 1);
}

}  // namespace
}  // namespace cortrix::spc
