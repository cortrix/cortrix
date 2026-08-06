// S3.3 — input length three-segment processing (reranker /):
//   NORMAL  → score normally
//   TRUNCATED (max_seq_length < passage ≤ 4×max_seq_length) → front-truncate +
//             LOG_WARN + cortrix_reranker_truncated_total++
//   EXTREMELY_LONG (passage > 4×max_seq_length) → score=0 + LOG_ERROR +
//             cortrix_reranker_extremely_long_total++
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/reranker/input_preprocessor.h"
#include "cortrix/reranker/onnx_reranker.h"
#include "cortrix/reranker/reranker_metrics.h"

namespace cortrix::reranker {
namespace {

// Build a string of `n` single-char ASCII "words" separated by spaces. Each word
// has len 1 → (1+2)/3 = 1 token, so the result is ~`n` tokens under the
// ApproxTokenCount heuristic (whitespace contributes 0). Lets tests hit category
// boundaries deterministically.
std::string NWordTokens(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        if (i) s += ' ';
        s += 'w';
    }
    return s;
}

// --- ApproxTokenCount sanity (the heuristic tests rely on) ---

TEST(InputPreprocessorTest, ApproxTokenCountMatchesHeuristic) {
    EXPECT_EQ(InputPreprocessor::ApproxTokenCount(""), 0);
    EXPECT_EQ(InputPreprocessor::ApproxTokenCount(NWordTokens(10)), 10);
    EXPECT_EQ(InputPreprocessor::ApproxTokenCount(NWordTokens(124)), 124);
}

// --- CategorizeInput: the three segments + the boundary tokens ---

TEST(InputPreprocessorTest, NormalWhenWithinBudget) {
    // max_seq_length=128, query=1 token → budget = 128-1-3 = 124.
    EXPECT_EQ(CategorizeInput(1, 124, 128), InputCategory::kNormal);
    EXPECT_EQ(CategorizeInput(1, 1, 128), InputCategory::kNormal);
}

TEST(InputPreprocessorTest, TruncatedJustAboveBudget) {
    // 125 > 124 budget, but 125 ≤ 4×128=512 → TRUNCATED.
    EXPECT_EQ(CategorizeInput(1, 125, 128), InputCategory::kTruncated);
    // Upper edge of TRUNCATED: exactly 4×max_seq_length.
    EXPECT_EQ(CategorizeInput(1, 512, 128), InputCategory::kTruncated);
}

TEST(InputPreprocessorTest, ExtremelyLongAboveFourX) {
    // 513 > 4×128=512 → EXTREMELY_LONG.
    EXPECT_EQ(CategorizeInput(1, 513, 128), InputCategory::kExtremelyLong);
}

TEST(InputPreprocessorTest, PassageBudgetFlooredAtZero) {
    // Query alone overflows the window → budget 0, any passage escalates.
    EXPECT_EQ(PassageTokenBudget(200, 128), 0);
    EXPECT_EQ(CategorizeInput(200, 1, 128), InputCategory::kTruncated);
}

// --- TruncateToTokens preserves the front, UTF-8 safe ---

TEST(InputPreprocessorTest, TruncateKeepsFrontTokens) {
    std::string text = NWordTokens(200);                 // ~200 tokens
    std::string cut = InputPreprocessor::TruncateToTokens(text, 50);
    EXPECT_LE(InputPreprocessor::ApproxTokenCount(cut), 50);
    EXPECT_GT(cut.size(), 0u);
    // Prefix preserved (front truncation).
    EXPECT_EQ(text.compare(0, cut.size(), cut), 0);
}

TEST(InputPreprocessorTest, TruncateUtf8Safe) {
    // 10 CJK chars (3 bytes / 2 tokens each = 20 tokens). Truncate to 6 tokens
    // (= 3 chars); the cut must not split a multi-byte code point.
    std::string cjk;
    for (int i = 0; i < 10; ++i) cjk += "\xe4\xb8\xad";  // U+4E2D (CJK char)
    std::string cut = InputPreprocessor::TruncateToTokens(cjk, 6);
    EXPECT_EQ(cut.size() % 3, 0u);  // whole CJK chars only (no split code points)
    EXPECT_LE(InputPreprocessor::ApproxTokenCount(cut), 6);
}

TEST(InputPreprocessorTest, TruncateZeroBudgetIsEmpty) {
    EXPECT_TRUE(InputPreprocessor::TruncateToTokens(NWordTokens(10), 0).empty());
}

// --- Prepare drives metrics + categories ---

TEST(InputPreprocessorTest, PrepareNormalIsPassthroughNoMetric) {
    RerankerMetrics::Instance().ResetForTest();
    PreprocessedPassage pre =
        InputPreprocessor::Prepare(NWordTokens(1), NWordTokens(100), 128);
    EXPECT_EQ(pre.category, InputCategory::kNormal);
    EXPECT_FALSE(pre.force_zero);
    EXPECT_EQ(pre.text, NWordTokens(100));
    EXPECT_EQ(RerankerMetrics::Instance().TruncatedCount(), 0u);
    EXPECT_EQ(RerankerMetrics::Instance().ExtremelyLongCount(), 0u);
}

TEST(InputPreprocessorTest, PrepareTruncatedRecordsMetricAndShrinks) {
    RerankerMetrics::Instance().ResetForTest();
    PreprocessedPassage pre =
        InputPreprocessor::Prepare(NWordTokens(1), NWordTokens(300), 128);
    EXPECT_EQ(pre.category, InputCategory::kTruncated);
    EXPECT_FALSE(pre.force_zero);
    EXPECT_LE(InputPreprocessor::ApproxTokenCount(pre.text), 124);  // within budget
    EXPECT_EQ(RerankerMetrics::Instance().TruncatedCount(), 1u);
    EXPECT_EQ(RerankerMetrics::Instance().ExtremelyLongCount(), 0u);
}

TEST(InputPreprocessorTest, PrepareExtremelyLongForcesZeroAndRecordsMetric) {
    RerankerMetrics::Instance().ResetForTest();
    PreprocessedPassage pre =
        InputPreprocessor::Prepare(NWordTokens(1), NWordTokens(600), 128);  // > 512
    EXPECT_EQ(pre.category, InputCategory::kExtremelyLong);
    EXPECT_TRUE(pre.force_zero);
    EXPECT_TRUE(pre.text.empty());
    EXPECT_EQ(RerankerMetrics::Instance().ExtremelyLongCount(), 1u);
    EXPECT_EQ(RerankerMetrics::Instance().TruncatedCount(), 0u);
}

// --- ScoreBatch integrates the policy: extremely-long candidate → score 0 ---

TEST(InputPreprocessorTest, ScoreBatchExtremelyLongPassageScoresZero) {
    RerankerMetrics::Instance().ResetForTest();
    RerankerConfig c;            // stub mode, max_seq_length=512
    OnnxReranker r(c, nullptr);
    ASSERT_TRUE(r.Init().ok());

    std::string normal = NWordTokens(50);
    std::string huge = NWordTokens(4 * 512 + 100);  // > 4×512 → EXTREMELY_LONG
    std::vector<const char*> passages{normal.c_str(), huge.c_str()};
    std::vector<float> scores = r.ScoreBatch("query", passages);

    ASSERT_EQ(scores.size(), 2u);
    EXPECT_EQ(scores[1], 0.0f);  // extremely-long candidate forced to 0
    EXPECT_EQ(RerankerMetrics::Instance().ExtremelyLongCount(), 1u);
}

TEST(InputPreprocessorTest, RenderIncludesNewCounters) {
    RerankerMetrics::Instance().ResetForTest();
    RerankerMetrics::Instance().RecordTruncated();
    RerankerMetrics::Instance().RecordExtremelyLong();
    std::string text = RerankerMetrics::Instance().RenderOpenMetrics();
    EXPECT_NE(text.find("cortrix_reranker_truncated_total 1"), std::string::npos);
    EXPECT_NE(text.find("cortrix_reranker_extremely_long_total 1"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::reranker
