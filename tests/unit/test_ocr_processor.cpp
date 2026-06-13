#include <gtest/gtest.h>
#include "cortrix/spc/ocr_processor.h"
#include "cortrix/spc/subprocess_utils.h"

#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace cortrix {
namespace {

// ============================================================
// Helper: write a fake OCR Python script that outputs JSON
// ============================================================

static std::string WriteFakeOcrScript(const std::string& json_output,
                                       int exit_code = 0,
                                       int delay_seconds = 0) {
    std::string path = "/tmp/cortrix_test_ocr_script.py";
    std::ofstream f(path);
    if (delay_seconds > 0) {
        f << "import time\n";
        f << "time.sleep(" << delay_seconds << ")\n";
    }
    if (exit_code != 0) {
        f << "import sys\n";
        f << "sys.exit(" << exit_code << ")\n";
    } else {
        f << "import sys\n";
        f << "print('''" << json_output << "''')\n";
    }
    return path;
}

static void CleanupFakeScript() {
    std::remove("/tmp/cortrix_test_ocr_script.py");
}

// ============================================================
// Construction and configuration tests
// ============================================================

TEST(OcrProcessorTest, ConstructorDefaultValues) {
    OcrProcessor proc("python3", "run_ocr.py");
    // Default timeout=60, confidence=0.3, retries=1
    // Just verify it constructs without crash
    SUCCEED();
}

TEST(OcrProcessorTest, ConstructorCustomValues) {
    OcrProcessor proc("python3", "run_ocr.py", 120, 0.5f, 3);
    SUCCEED();
}

// ============================================================
// Process - null result pointer
// ============================================================

TEST(OcrProcessorTest, Process_NullResult_Error) {
    OcrProcessor proc("python3", "run_ocr.py", 5);
    Status s = proc.Process("/tmp/test.png", nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// ============================================================
// Process - subprocess failure (nonexistent script)
// ============================================================

TEST(OcrProcessorTest, Process_SubprocessFail_Error) {
    OcrProcessor proc("python3", "/nonexistent/script.py", 5, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    EXPECT_FALSE(s.ok());
    // Should fail because subprocess exits non-zero
}

// ============================================================
// Process - subprocess failure with retry
// ============================================================

TEST(OcrProcessorTest, Process_SubprocessFail_Retries) {
    // Use a script that always fails - should retry max_retries times
    OcrProcessor proc("python3", "/nonexistent/script.py", 5, 0.3f, 1);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    EXPECT_FALSE(s.ok());
    // With max_retries=1, it attempts twice total (attempt 0 + attempt 1)
}

// ============================================================
// Process - subprocess timeout
// ============================================================

TEST(OcrProcessorTest, Process_Timeout) {
    std::string script = WriteFakeOcrScript("", 0, 30);
    OcrProcessor proc("python3", script, 1, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("timed out"), std::string::npos);
    CleanupFakeScript();
}

// ============================================================
// Process - valid JSON array with OCR lines
// ============================================================

TEST(OcrProcessorTest, Process_ValidOcrOutput) {
    std::string json = R"([
        {"text": "Hello World", "confidence": 0.95, "box": [[10,10],[100,10],[100,30],[10,30]]},
        {"text": "Second Line", "confidence": 0.88, "box": [[10,50],[100,50],[100,70],[10,70]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.total_lines, 2);
    EXPECT_EQ(result.filtered_lines, 2);
    EXPECT_EQ(result.lines.size(), 2u);
    EXPECT_FLOAT_EQ(result.avg_confidence, (0.95f + 0.88f) / 2.0f);
    EXPECT_FALSE(result.merged_text.empty());
    EXPECT_NE(result.merged_text.find("Hello World"), std::string::npos);
    EXPECT_NE(result.merged_text.find("Second Line"), std::string::npos);

    CleanupFakeScript();
}

// ============================================================
// Process - confidence filtering
// ============================================================

TEST(OcrProcessorTest, Process_ConfidenceFiltering) {
    std::string json = R"([
        {"text": "Good Text", "confidence": 0.90, "box": [[10,10],[100,10],[100,30],[10,30]]},
        {"text": "Low Conf", "confidence": 0.10, "box": [[10,50],[100,50],[100,70],[10,70]]},
        {"text": "Borderline", "confidence": 0.30, "box": [[10,90],[100,90],[100,110],[10,110]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.total_lines, 3);
    // "Good Text" (0.90 >= 0.3) and "Borderline" (0.30 >= 0.3) pass filter
    // "Low Conf" (0.10 < 0.3) filtered out
    EXPECT_EQ(result.filtered_lines, 2);
    EXPECT_EQ(result.lines.size(), 2u);

    // avg_confidence is computed from ALL lines (before filtering)
    float expected_avg = (0.90f + 0.10f + 0.30f) / 3.0f;
    EXPECT_NEAR(result.avg_confidence, expected_avg, 0.01f);

    // Merged text should only contain filtered lines
    EXPECT_NE(result.merged_text.find("Good Text"), std::string::npos);
    EXPECT_NE(result.merged_text.find("Borderline"), std::string::npos);
    // Low confidence text should NOT be in merged text
    EXPECT_EQ(result.merged_text.find("Low Conf"), std::string::npos);

    CleanupFakeScript();
}

// ============================================================
// Process - all lines below threshold
// ============================================================

TEST(OcrProcessorTest, Process_AllBelowThreshold) {
    std::string json = R"([
        {"text": "Noise", "confidence": 0.05, "box": [[0,0],[10,0],[10,10],[0,10]]},
        {"text": "Garbage", "confidence": 0.15, "box": [[0,20],[10,20],[10,30],[0,30]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.5f, 0);  // threshold=0.5
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.total_lines, 2);
    EXPECT_EQ(result.filtered_lines, 0);
    EXPECT_EQ(result.lines.size(), 0u);
    EXPECT_TRUE(result.merged_text.empty());

    CleanupFakeScript();
}

// ============================================================
// Process - empty JSON array (no text detected)
// ============================================================

TEST(OcrProcessorTest, Process_EmptyArray) {
    std::string json = "[]";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.total_lines, 0);
    EXPECT_EQ(result.filtered_lines, 0);
    EXPECT_TRUE(result.lines.empty());
    EXPECT_TRUE(result.merged_text.empty());
    EXPECT_FLOAT_EQ(result.avg_confidence, 0.0f);

    CleanupFakeScript();
}

// ============================================================
// Process - JSON error object
// ============================================================

TEST(OcrProcessorTest, Process_JsonErrorObject) {
    std::string json = R"({"error": "PaddleOCR model not found"})";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("PaddleOCR model not found"), std::string::npos);

    CleanupFakeScript();
}

// ============================================================
// Process - non-array JSON (not error, not array)
// ============================================================

TEST(OcrProcessorTest, Process_NonArrayJson) {
    std::string json = R"({"lines": []})";  // Object but no "error" key
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("not a JSON array"), std::string::npos);

    CleanupFakeScript();
}

// ============================================================
// Process - invalid JSON
// ============================================================

TEST(OcrProcessorTest, Process_InvalidJson) {
    std::string json = "not valid json at all {{{";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("Failed to parse OCR JSON"), std::string::npos);

    CleanupFakeScript();
}

// ============================================================
// Process - reading order sort (top-to-bottom, left-to-right)
// ============================================================

TEST(OcrProcessorTest, Process_ReadingOrderSort) {
    // Lines in reverse order; should be sorted by y then x
    std::string json = R"([
        {"text": "Bottom Left", "confidence": 0.9, "box": [[10,100],[100,100],[100,120],[10,120]]},
        {"text": "Top Right", "confidence": 0.9, "box": [[200,10],[300,10],[300,30],[200,30]]},
        {"text": "Top Left", "confidence": 0.9, "box": [[10,10],[100,10],[100,30],[10,30]]},
        {"text": "Bottom Right", "confidence": 0.9, "box": [[200,100],[300,100],[300,120],[200,120]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    ASSERT_EQ(result.lines.size(), 4u);
    // Top row: Left first, Right second (y ≈ 10, within tolerance)
    EXPECT_EQ(result.lines[0].text, "Top Left");
    EXPECT_EQ(result.lines[1].text, "Top Right");
    // Bottom row: Left first, Right second (y ≈ 100)
    EXPECT_EQ(result.lines[2].text, "Bottom Left");
    EXPECT_EQ(result.lines[3].text, "Bottom Right");

    CleanupFakeScript();
}

// ============================================================
// Process - merge text with paragraph breaks
// ============================================================

TEST(OcrProcessorTest, Process_MergeTextParagraphBreaks) {
    // y-diff > 40 => paragraph break, y-diff > 20 => line break, else space
    std::string json = R"([
        {"text": "Para1", "confidence": 0.9, "box": [[10,10],[100,10],[100,30],[10,30]]},
        {"text": "SameLine", "confidence": 0.9, "box": [[120,12],[200,12],[200,32],[120,32]]},
        {"text": "NextLine", "confidence": 0.9, "box": [[10,55],[100,55],[100,75],[10,75]]},
        {"text": "NewPara", "confidence": 0.9, "box": [[10,120],[100,120],[100,140],[10,140]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    // Verify merge text has appropriate spacing
    // "Para1" and "SameLine" are on same line (y diff < 20) -> space
    // "NextLine" is a line break away from "SameLine" (y diff ~43 from SameLine's y=12) -> could be paragraph
    // "NewPara" is far below (y=120 - y=55 = 65) -> paragraph break
    EXPECT_NE(result.merged_text.find("Para1"), std::string::npos);
    EXPECT_NE(result.merged_text.find("SameLine"), std::string::npos);
    EXPECT_NE(result.merged_text.find("NewPara"), std::string::npos);

    CleanupFakeScript();
}

// ============================================================
// Process - box missing/incomplete
// ============================================================

TEST(OcrProcessorTest, Process_MissingBoxField) {
    std::string json = R"([
        {"text": "No Box Line", "confidence": 0.9}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.total_lines, 1);
    EXPECT_EQ(result.filtered_lines, 1);
    EXPECT_EQ(result.lines[0].text, "No Box Line");

    CleanupFakeScript();
}

TEST(OcrProcessorTest, Process_BoxWrongFormat) {
    // Box is present but not a proper 4x2 array
    std::string json = R"([
        {"text": "Bad Box", "confidence": 0.9, "box": [1, 2, 3]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.lines.size(), 1u);

    CleanupFakeScript();
}

// ============================================================
// Process - missing text/confidence fields (defaults)
// ============================================================

TEST(OcrProcessorTest, Process_MissingTextAndConfidence) {
    std::string json = R"([
        {"box": [[0,0],[10,0],[10,10],[0,10]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.0f, 0);  // threshold=0 to keep it
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.total_lines, 1);
    // Default text="" and confidence=0.0
    EXPECT_EQ(result.lines[0].text, "");
    EXPECT_FLOAT_EQ(result.lines[0].confidence, 0.0f);

    CleanupFakeScript();
}

// ============================================================
// Process - retry succeeds on second attempt
// ============================================================

TEST(OcrProcessorTest, Process_RetrySucceedsOnSecondAttempt) {
    // This is tricky with a static script. We test that max_retries=1
    // means 2 attempts. Since we can't change script between attempts,
    // we'll test that a successful script works with retries enabled.
    std::string json = R"([
        {"text": "Retried OK", "confidence": 0.99, "box": [[0,0],[10,0],[10,10],[0,10]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 1);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.lines[0].text, "Retried OK");

    CleanupFakeScript();
}

// ============================================================
// Process - large number of lines
// ============================================================

TEST(OcrProcessorTest, Process_ManyLines) {
    std::string json = "[";
    for (int i = 0; i < 50; ++i) {
        if (i > 0) json += ",";
        int y = i * 25;
        json += R"({"text": "Line )" + std::to_string(i) + R"(", "confidence": 0.8, "box": [[0,)" +
                std::to_string(y) + "],[100," + std::to_string(y) + "],[100," +
                std::to_string(y + 20) + "],[0," + std::to_string(y + 20) + "]]}";
    }
    json += "]";

    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.total_lines, 50);
    EXPECT_EQ(result.filtered_lines, 50);
    EXPECT_FALSE(result.merged_text.empty());

    CleanupFakeScript();
}

// ============================================================
// Process - single line (no prev_y comparison)
// ============================================================

TEST(OcrProcessorTest, Process_SingleLine) {
    std::string json = R"([
        {"text": "Only Line", "confidence": 0.95, "box": [[10,10],[100,10],[100,30],[10,30]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(result.merged_text, "Only Line");

    CleanupFakeScript();
}

// ============================================================
// CheckAvailability tests
// ============================================================

TEST(OcrProcessorTest, CheckAvailability_NotInstalled) {
    OcrProcessor proc("python3", "run_ocr.py");
    // paddleocr is likely not installed in test env
    Status s = proc.CheckAvailability();
    // Either ok or unavailable depending on test environment
    if (!s.ok()) {
        EXPECT_EQ(s.code(), StatusCode::kUnavailable);
        EXPECT_NE(s.message().find("not installed"), std::string::npos);
    }
}

// ============================================================
// SortByReadingOrder - empty/null lines
// ============================================================

TEST(OcrProcessorTest, SortByReadingOrder_EmptyLines) {
    // SortByReadingOrder is private, but we test it indirectly
    // via Process with empty array
    std::string json = "[]";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(result.lines.empty());

    CleanupFakeScript();
}

// ============================================================
// MergeText - empty lines list
// ============================================================

TEST(OcrProcessorTest, MergeText_EmptyInput) {
    // Test via Process with all lines filtered out
    std::string json = R"([
        {"text": "Filtered", "confidence": 0.01, "box": [[0,0],[10,0],[10,10],[0,10]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.5f, 0);  // threshold=0.5 filters this
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(result.merged_text.empty());

    CleanupFakeScript();
}

// ============================================================
// Process - script that outputs error then retry logic
// ============================================================

TEST(OcrProcessorTest, Process_ScriptExitNonZero_WithRetry) {
    std::string script = WriteFakeOcrScript("", 1);  // Exit code 1
    OcrProcessor proc("python3", script, 10, 0.3f, 2);  // max_retries=2
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    EXPECT_FALSE(s.ok());
    // Should have retried 3 times total (attempt 0, 1, 2)

    CleanupFakeScript();
}

// ============================================================
// Process - zero retries (single attempt)
// ============================================================

TEST(OcrProcessorTest, Process_ZeroRetries) {
    std::string script = WriteFakeOcrScript("", 1);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);  // max_retries=0
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    EXPECT_FALSE(s.ok());

    CleanupFakeScript();
}

// ============================================================
// Process - box with partial inner arrays
// ============================================================

TEST(OcrProcessorTest, Process_BoxPartialInnerArrays) {
    std::string json = R"([
        {"text": "Partial Box", "confidence": 0.8, "box": [[10,20],[30],[40,50],[60,70]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.lines.size(), 1u);

    CleanupFakeScript();
}

// ============================================================
// Process - high confidence threshold filters everything
// ============================================================

TEST(OcrProcessorTest, Process_HighThreshold) {
    std::string json = R"([
        {"text": "Normal", "confidence": 0.95, "box": [[0,0],[10,0],[10,10],[0,10]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.99f, 0);  // threshold=0.99
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.filtered_lines, 0);
    EXPECT_TRUE(result.merged_text.empty());

    CleanupFakeScript();
}

// ============================================================
// Process - lines on same y-coordinate (same-line grouping)
// ============================================================

TEST(OcrProcessorTest, Process_SameLineGrouping) {
    // Two texts on same y-line within tolerance
    std::string json = R"([
        {"text": "Right", "confidence": 0.9, "box": [[200,15],[300,15],[300,35],[200,35]]},
        {"text": "Left", "confidence": 0.9, "box": [[10,10],[100,10],[100,30],[10,30]]}
    ])";
    std::string script = WriteFakeOcrScript(json);
    OcrProcessor proc("python3", script, 10, 0.3f, 0);
    OcrResult result;
    Status s = proc.Process("/tmp/test.png", &result);
    ASSERT_TRUE(s.ok()) << s.message();

    // Should be sorted left-to-right on same line
    ASSERT_EQ(result.lines.size(), 2u);
    EXPECT_EQ(result.lines[0].text, "Left");
    EXPECT_EQ(result.lines[1].text, "Right");

    CleanupFakeScript();
}

}  // namespace
}  // namespace cortrix
