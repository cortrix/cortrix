#include <gtest/gtest.h>
#include "cortrix/spc/spc_router.h"
#include "cortrix/common/block_types.h"

namespace cortrix {
namespace {

// ============================================================
// InferMimeType - document types
// ============================================================

TEST(SPCRouterStandaloneTest, InferMimeType_Pdf) {
    EXPECT_EQ(SPCRouter::InferMimeType("report.pdf"), "application/pdf");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Docx) {
    EXPECT_EQ(SPCRouter::InferMimeType("document.docx"),
              "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Doc) {
    EXPECT_EQ(SPCRouter::InferMimeType("old.doc"), "application/msword");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Xlsx) {
    EXPECT_EQ(SPCRouter::InferMimeType("data.xlsx"),
              "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Xls) {
    EXPECT_EQ(SPCRouter::InferMimeType("data.xls"), "application/vnd.ms-excel");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Pptx) {
    EXPECT_EQ(SPCRouter::InferMimeType("slides.pptx"),
              "application/vnd.openxmlformats-officedocument.presentationml.presentation");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Markdown_md) {
    EXPECT_EQ(SPCRouter::InferMimeType("README.md"), "text/markdown");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Markdown_long_ext) {
    EXPECT_EQ(SPCRouter::InferMimeType("notes.markdown"), "text/markdown");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Txt) {
    EXPECT_EQ(SPCRouter::InferMimeType("notes.txt"), "text/plain");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Csv) {
    EXPECT_EQ(SPCRouter::InferMimeType("data.csv"), "text/csv");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Html) {
    EXPECT_EQ(SPCRouter::InferMimeType("page.html"), "text/html");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Htm) {
    EXPECT_EQ(SPCRouter::InferMimeType("page.htm"), "text/html");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Json) {
    EXPECT_EQ(SPCRouter::InferMimeType("config.json"), "application/json");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Xml) {
    EXPECT_EQ(SPCRouter::InferMimeType("feed.xml"), "application/xml");
}

// ============================================================
// InferMimeType - image types
// ============================================================

TEST(SPCRouterStandaloneTest, InferMimeType_Png) {
    EXPECT_EQ(SPCRouter::InferMimeType("photo.png"), "image/png");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Jpg) {
    EXPECT_EQ(SPCRouter::InferMimeType("photo.jpg"), "image/jpeg");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Jpeg) {
    EXPECT_EQ(SPCRouter::InferMimeType("photo.jpeg"), "image/jpeg");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Tiff) {
    EXPECT_EQ(SPCRouter::InferMimeType("scan.tiff"), "image/tiff");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Tif) {
    EXPECT_EQ(SPCRouter::InferMimeType("scan.tif"), "image/tiff");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Bmp) {
    EXPECT_EQ(SPCRouter::InferMimeType("image.bmp"), "image/bmp");
}

// ============================================================
// InferMimeType - L0 skip types
// ============================================================

TEST(SPCRouterStandaloneTest, InferMimeType_Tmp) {
    EXPECT_EQ(SPCRouter::InferMimeType("cache.tmp"), "application/x-temp");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Bak) {
    EXPECT_EQ(SPCRouter::InferMimeType("backup.bak"), "application/x-temp");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Exe) {
    EXPECT_EQ(SPCRouter::InferMimeType("program.exe"), "application/x-executable");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Dll) {
    EXPECT_EQ(SPCRouter::InferMimeType("lib.dll"), "application/x-executable");
}

TEST(SPCRouterStandaloneTest, InferMimeType_So) {
    EXPECT_EQ(SPCRouter::InferMimeType("lib.so"), "application/x-executable");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Ttf) {
    EXPECT_EQ(SPCRouter::InferMimeType("font.ttf"), "font/ttf");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Otf) {
    EXPECT_EQ(SPCRouter::InferMimeType("font.otf"), "font/ttf");
}

TEST(SPCRouterStandaloneTest, InferMimeType_Woff) {
    EXPECT_EQ(SPCRouter::InferMimeType("font.woff"), "font/ttf");
}

// ============================================================
// InferMimeType - edge cases
// ============================================================

TEST(SPCRouterStandaloneTest, InferMimeType_NoExtension) {
    EXPECT_EQ(SPCRouter::InferMimeType("README"), "application/octet-stream");
}

TEST(SPCRouterStandaloneTest, InferMimeType_UnknownExtension) {
    EXPECT_EQ(SPCRouter::InferMimeType("data.xyz"), "application/octet-stream");
}

TEST(SPCRouterStandaloneTest, InferMimeType_UppercaseExtension) {
    EXPECT_EQ(SPCRouter::InferMimeType("REPORT.PDF"), "application/pdf");
}

TEST(SPCRouterStandaloneTest, InferMimeType_MixedCaseExtension) {
    EXPECT_EQ(SPCRouter::InferMimeType("Notes.TxT"), "text/plain");
}

TEST(SPCRouterStandaloneTest, InferMimeType_DotOnly) {
    // Filename is just a dot
    EXPECT_EQ(SPCRouter::InferMimeType("."), "application/octet-stream");
}

TEST(SPCRouterStandaloneTest, InferMimeType_MultipleDots) {
    EXPECT_EQ(SPCRouter::InferMimeType("archive.tar.gz"), "application/octet-stream");
}

TEST(SPCRouterStandaloneTest, InferMimeType_HiddenFilePdf) {
    EXPECT_EQ(SPCRouter::InferMimeType(".hidden.pdf"), "application/pdf");
}

TEST(SPCRouterStandaloneTest, InferMimeType_EmptyString) {
    EXPECT_EQ(SPCRouter::InferMimeType(""), "application/octet-stream");
}

TEST(SPCRouterStandaloneTest, InferMimeType_PathWithDirectories) {
    EXPECT_EQ(SPCRouter::InferMimeType("/path/to/report.pdf"), "application/pdf");
}

// ============================================================
// IsSupported
// ============================================================

TEST(SPCRouterStandaloneTest, IsSupported_Pdf) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/pdf"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Docx) {
    EXPECT_TRUE(SPCRouter::IsSupported(
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document"));
}

TEST(SPCRouterStandaloneTest, IsSupported_MsWord) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/msword"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Xlsx) {
    EXPECT_TRUE(SPCRouter::IsSupported(
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Xls) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/vnd.ms-excel"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Markdown) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/markdown"));
}

TEST(SPCRouterStandaloneTest, IsSupported_TextPlain) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/plain"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Csv) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/csv"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Html) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/html"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Json) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/json"));
}

TEST(SPCRouterStandaloneTest, IsSupported_ApplicationXml) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/xml"));
}

TEST(SPCRouterStandaloneTest, IsSupported_TextXml) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/xml"));
}

TEST(SPCRouterStandaloneTest, IsSupported_ImagePng) {
    EXPECT_TRUE(SPCRouter::IsSupported("image/png"));
}

TEST(SPCRouterStandaloneTest, IsSupported_ImageJpeg) {
    EXPECT_TRUE(SPCRouter::IsSupported("image/jpeg"));
}

TEST(SPCRouterStandaloneTest, IsSupported_ImageTiff) {
    EXPECT_TRUE(SPCRouter::IsSupported("image/tiff"));
}

TEST(SPCRouterStandaloneTest, IsSupported_ImageBmp) {
    EXPECT_TRUE(SPCRouter::IsSupported("image/bmp"));
}

// Not supported types
TEST(SPCRouterStandaloneTest, IsSupported_VideoMp4_No) {
    EXPECT_FALSE(SPCRouter::IsSupported("video/mp4"));
}

TEST(SPCRouterStandaloneTest, IsSupported_AudioMp3_No) {
    EXPECT_FALSE(SPCRouter::IsSupported("audio/mpeg"));
}

TEST(SPCRouterStandaloneTest, IsSupported_OctetStream_No) {
    EXPECT_FALSE(SPCRouter::IsSupported("application/octet-stream"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Empty_No) {
    EXPECT_FALSE(SPCRouter::IsSupported(""));
}

TEST(SPCRouterStandaloneTest, IsSupported_Temp_No) {
    EXPECT_FALSE(SPCRouter::IsSupported("application/x-temp"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Executable_No) {
    EXPECT_FALSE(SPCRouter::IsSupported("application/x-executable"));
}

TEST(SPCRouterStandaloneTest, IsSupported_Font_No) {
    EXPECT_FALSE(SPCRouter::IsSupported("font/ttf"));
}

// PPTX IsSupported - was missing, now fixed (design gap closed)
TEST(SPCRouterStandaloneTest, IsSupported_Pptx) {
    EXPECT_TRUE(SPCRouter::IsSupported(
        "application/vnd.openxmlformats-officedocument.presentationml.presentation"));
}

// ============================================================
// InferBlockType
// ============================================================

TEST(SPCRouterStandaloneTest, InferBlockType_ImagePng_Scan) {
    EXPECT_EQ(SPCRouter::InferBlockType("image/png"), kBlockScan);
}

TEST(SPCRouterStandaloneTest, InferBlockType_ImageJpeg_Scan) {
    EXPECT_EQ(SPCRouter::InferBlockType("image/jpeg"), kBlockScan);
}

TEST(SPCRouterStandaloneTest, InferBlockType_ImageTiff_Scan) {
    EXPECT_EQ(SPCRouter::InferBlockType("image/tiff"), kBlockScan);
}

TEST(SPCRouterStandaloneTest, InferBlockType_ImageBmp_Scan) {
    EXPECT_EQ(SPCRouter::InferBlockType("image/bmp"), kBlockScan);
}

TEST(SPCRouterStandaloneTest, InferBlockType_Pdf_File) {
    EXPECT_EQ(SPCRouter::InferBlockType("application/pdf"), kBlockFile);
}

TEST(SPCRouterStandaloneTest, InferBlockType_TextPlain_File) {
    EXPECT_EQ(SPCRouter::InferBlockType("text/plain"), kBlockFile);
}

TEST(SPCRouterStandaloneTest, InferBlockType_Docx_File) {
    EXPECT_EQ(SPCRouter::InferBlockType(
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document"),
        kBlockFile);
}

TEST(SPCRouterStandaloneTest, InferBlockType_Unknown_File) {
    EXPECT_EQ(SPCRouter::InferBlockType("application/octet-stream"), kBlockFile);
}

// ============================================================
// NeedsOcr
// ============================================================

TEST(SPCRouterStandaloneTest, NeedsOcr_ImagePng_Always) {
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/png", true));
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/png", false));
}

TEST(SPCRouterStandaloneTest, NeedsOcr_ImageJpeg_Always) {
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/jpeg", true));
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/jpeg", false));
}

TEST(SPCRouterStandaloneTest, NeedsOcr_ImageTiff_Always) {
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/tiff", true));
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/tiff", false));
}

TEST(SPCRouterStandaloneTest, NeedsOcr_ImageBmp_Always) {
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/bmp", true));
    EXPECT_TRUE(SPCRouter::NeedsOcr("image/bmp", false));
}

TEST(SPCRouterStandaloneTest, NeedsOcr_PdfWithText_No) {
    EXPECT_FALSE(SPCRouter::NeedsOcr("application/pdf", true));
}

TEST(SPCRouterStandaloneTest, NeedsOcr_PdfWithoutText_Yes) {
    // PROBE mechanism: image-only PDF needs OCR
    EXPECT_TRUE(SPCRouter::NeedsOcr("application/pdf", false));
}

TEST(SPCRouterStandaloneTest, NeedsOcr_TextPlain_No) {
    EXPECT_FALSE(SPCRouter::NeedsOcr("text/plain", false));
    EXPECT_FALSE(SPCRouter::NeedsOcr("text/plain", true));
}

TEST(SPCRouterStandaloneTest, NeedsOcr_Docx_No) {
    EXPECT_FALSE(SPCRouter::NeedsOcr(
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document", false));
}

TEST(SPCRouterStandaloneTest, NeedsOcr_Markdown_No) {
    EXPECT_FALSE(SPCRouter::NeedsOcr("text/markdown", false));
}

// ============================================================
// InferProcessingLevel
// ============================================================

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Temp_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/x-temp"), 0);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Executable_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/x-executable"), 0);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Font_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("font/ttf"), 0);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Pdf_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/pdf"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_TextPlain_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("text/plain"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_ImagePng_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("image/png"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_ImageJpeg_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("image/jpeg"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Markdown_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("text/markdown"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Csv_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("text/csv"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Html_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("text/html"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Json_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/json"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Unknown_L3) {
    // Unknown MIME types default to L3
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/octet-stream"), 3);
}

TEST(SPCRouterStandaloneTest, InferProcessingLevel_Empty_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel(""), 3);
}

}  // namespace
}  // namespace cortrix
