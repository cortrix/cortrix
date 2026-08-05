#include <gtest/gtest.h>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/block_types.h"
#include "cortrix/scoring/score_map.h"
#include "cortrix/scoring/scoring_error.h"

// Semantic score S1 + S2 / §7.1 coverage: ScoreMap.ComputeLevel (D2 Matrix multi-dimensional decision, 8 cases) +
// LevelToScore (5 discrete levels + out-of-range exception). The core scoring algorithm.
namespace cortrix::scoring {
namespace {

ScoringInput Input(const std::string& parser, const std::string& enricher,
                   bool image = false, uint16_t block_type = kBlockFile) {
    ScoringInput in;
    in.parser_name = parser;
    in.enricher_name = enricher;
    in.has_image_caption = image;
    in.block_type = block_type;
    return in;
}

// --- ScoreMap.ComputeLevel (Matrix max over parser/enricher/image) ---

TEST(ScoreMapComputeLevelTest, DoclingPlusNullIsLevel2) {
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("docling", "null")), 2);
}

TEST(ScoreMapComputeLevelTest, DoclingPlusLlmIsLevel4) {
    // enricher_level(llm)=4 dominates parser_level(docling)=2.
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("docling", "llm")), 4);
}

TEST(ScoreMapComputeLevelTest, PaddleOcrPlusNullIsLevel1) {
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("paddleocr", "null")), 1);
}

TEST(ScoreMapComputeLevelTest, DoclingPlusContextualIsLevel3) {
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("docling", "contextual")), 3);
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("docling", "hype")), 3);
}

TEST(ScoreMapComputeLevelTest, ImageCaptionForcesLevel4) {
    // image_level=4 dominates even paddleocr(1)+null(0).
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("paddleocr", "null", /*image=*/true)), 4);
}

TEST(ScoreMapComputeLevelTest, DoclingPlusPaddleOcrIsLevel2) {
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("docling+paddleocr", "null")), 2);
}

TEST(ScoreMapComputeLevelTest, EmptyOrUnknownInputBottomsOutLevel0) {
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("", "")), 0);
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("unknown_parser", "unknown_enricher")), 0);
}

// (v1.0.1 M1) Meta Block (block_type==8) → forced Level 0, regardless of parser/enricher/image.
TEST(ScoreMapComputeLevelTest, MetaBlockSpecialCaseForcesLevel0) {
    // Even with llm enricher + image (which would otherwise be Level 4), META → 0.
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("docling", "llm", /*image=*/true, kBlockMeta)), 0);
    EXPECT_EQ(ScoreMap::ComputeLevel(Input("docling", "null", false, kBlockMeta)), 0);
    // Sanity: the same non-META input is NOT 0.
    EXPECT_NE(ScoreMap::ComputeLevel(Input("docling", "llm", true, kBlockFile)), 0);
}

// ComputeLevel output is always in [0,4] (D3 strict 5 discrete levels — never out of range).
TEST(ScoreMapComputeLevelTest, OutputAlwaysInRange) {
    for (uint16_t bt : {kBlockFile, kBlockScan, kBlockMeta, kBlockImage}) {
        for (const char* p : {"docling", "paddleocr", "docling+paddleocr", "", "x"}) {
            for (const char* e : {"llm", "contextual", "hype", "null", "x"}) {
                uint8_t lv = ScoreMap::ComputeLevel(Input(p, e, false, bt));
                EXPECT_LE(lv, 4);
            }
        }
    }
}

// Sub-level helpers (targeted).
TEST(ScoreMapComputeLevelTest, SubLevelHelpers) {
    EXPECT_EQ(ScoreMap::ParserLevel("docling"), 2);
    EXPECT_EQ(ScoreMap::ParserLevel("docling+paddleocr"), 2);
    EXPECT_EQ(ScoreMap::ParserLevel("paddleocr"), 1);
    EXPECT_EQ(ScoreMap::ParserLevel("nope"), 0);
    EXPECT_EQ(ScoreMap::EnricherLevel("llm"), 4);
    EXPECT_EQ(ScoreMap::EnricherLevel("contextual"), 3);
    EXPECT_EQ(ScoreMap::EnricherLevel("hype"), 3);
    EXPECT_EQ(ScoreMap::EnricherLevel("null"), 0);
    EXPECT_EQ(ScoreMap::EnricherLevel("nope"), 0);
}

// --- ScoreMap.LevelToScore (5 discrete-level mapping + out-of-range exception) ---

TEST(ScoreMapLevelToScoreTest, FiveLevelDiscreteMapping) {
    EXPECT_FLOAT_EQ(ScoreMap::LevelToScore(0), 0.2f);
    EXPECT_FLOAT_EQ(ScoreMap::LevelToScore(1), 0.4f);
    EXPECT_FLOAT_EQ(ScoreMap::LevelToScore(2), 0.6f);
    EXPECT_FLOAT_EQ(ScoreMap::LevelToScore(3), 0.8f);
    EXPECT_FLOAT_EQ(ScoreMap::LevelToScore(4), 1.0f);
}

TEST(ScoreMapLevelToScoreTest, OutOfRangeThrowsLevelInvalid) {
    try {
        ScoreMap::LevelToScore(5);
        FAIL() << "expected AgentFriendlyException";
    } catch (const agent_friendly::AgentFriendlyException& e) {
        EXPECT_EQ(e.GetError().code, "CX_ERR_SCORING_LEVEL_INVALID");
        ASSERT_TRUE(e.GetError().structured_data.has_value());
        EXPECT_EQ((*e.GetError().structured_data)["max_allowed"], 4);
    }
    // 255 (uint8 max) also throws.
    EXPECT_THROW(ScoreMap::LevelToScore(255), agent_friendly::AgentFriendlyException);
}

// The static array is exactly the ARCH §5.1.2 SoT table.
TEST(ScoreMapLevelToScoreTest, KLevelScoreTableMatchesArchSoT) {
    EXPECT_FLOAT_EQ(ScoreMap::kLevelScore[0], 0.2f);
    EXPECT_FLOAT_EQ(ScoreMap::kLevelScore[4], 1.0f);
}

}  // namespace
}  // namespace cortrix::scoring
