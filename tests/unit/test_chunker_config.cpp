#include <gtest/gtest.h>

#include <string>

#include "cortrix/chunker/chunker_config.h"
#include "cortrix/chunker/chunker_guc.h"
#include "cortrix/chunker/chunker_startup_validator.h"
#include "cortrix/common/in_memory_global_config.h"

namespace cortrix::chunker {
namespace {

// --- ChunkerConfig enum <-> string ---------------------------------------------

TEST(ChunkerConfigTest, DefaultsMatchLockedValues) {
    ChunkerConfig c;
    EXPECT_EQ(c.strategy, ChunkStrategy::kParentChild);
    EXPECT_EQ(c.parent_size, 1024);
    EXPECT_EQ(c.child_size, 200);
    EXPECT_EQ(c.child_overlap, 20);              //
    EXPECT_EQ(c.unit_level, UnitLevel::kParagraph);  //
    EXPECT_EQ(c.max_parents_per_doc, 10000);     //
    EXPECT_EQ(c.fallback_to_flat_threshold, 10000);  //
    EXPECT_EQ(c.children_per_parent_for_rerank, 3);  //
}

TEST(ChunkerConfigTest, StrategyRoundTrip) {
    EXPECT_STREQ(ToString(ChunkStrategy::kParentChild), "parent-child");
    EXPECT_STREQ(ToString(ChunkStrategy::kFlat), "flat");
    EXPECT_STREQ(ToString(ChunkStrategy::kSemantic), "semantic");
    EXPECT_EQ(ChunkStrategyFromString("flat"), ChunkStrategy::kFlat);
    EXPECT_EQ(ChunkStrategyFromString("semantic"), ChunkStrategy::kSemantic);
    EXPECT_EQ(ChunkStrategyFromString("parent-child"), ChunkStrategy::kParentChild);
    EXPECT_EQ(ChunkStrategyFromString("nonsense"), ChunkStrategy::kParentChild);  // default
}

TEST(ChunkerConfigTest, UnitLevelRoundTrip) {
    EXPECT_STREQ(ToString(UnitLevel::kPage), "page");
    EXPECT_STREQ(ToString(UnitLevel::kParagraph), "paragraph");
    EXPECT_STREQ(ToString(UnitLevel::kSentence), "sentence");
    EXPECT_EQ(UnitLevelFromString("page"), UnitLevel::kPage);
    EXPECT_EQ(UnitLevelFromString("sentence"), UnitLevel::kSentence);
    EXPECT_EQ(UnitLevelFromString("nonsense"), UnitLevel::kParagraph);  // default
}

// --- ChunkerGuc range validation (REJECT policy) -------------------------------

TEST(ChunkerGucTest, DefaultConfigValidates) {
    EXPECT_TRUE(ChunkerGuc::ValidateConfig(ChunkerConfig{}).ok());
}

TEST(ChunkerGucTest, ParentSizeOutOfRangeRejected) {
    ChunkerConfig c;
    c.parent_size = 100;  // < 256
    Status s = ChunkerGuc::ValidateConfig(c);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("parent_size"), std::string::npos);
}

TEST(ChunkerGucTest, ChildOverlapOutOfRangeRejected) {
    ChunkerConfig c;
    c.child_overlap = 200;  // > 100
    EXPECT_FALSE(ChunkerGuc::ValidateConfig(c).ok());
}

TEST(ChunkerGucTest, ChildrenPerParentOutOfRangeRejected) {
    ChunkerConfig c;
    c.children_per_parent_for_rerank = 0;  // < 1
    EXPECT_FALSE(ChunkerGuc::ValidateConfig(c).ok());
    c.children_per_parent_for_rerank = 11;  // > 10
    EXPECT_FALSE(ChunkerGuc::ValidateConfig(c).ok());
}

TEST(ChunkerGucTest, ChildBiggerThanParentRejected) {
    ChunkerConfig c;
    c.parent_size = 256;
    c.child_size = 300;  // > parent_size
    Status s = ChunkerGuc::ValidateConfig(c);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("child_size"), std::string::npos);
}

TEST(ChunkerGucTest, ValidateRangeBoundariesInclusive) {
    EXPECT_TRUE(ChunkerGuc::ValidateRange("k", 100, 100, 200).ok());
    EXPECT_TRUE(ChunkerGuc::ValidateRange("k", 200, 100, 200).ok());
    EXPECT_FALSE(ChunkerGuc::ValidateRange("k", 99, 100, 200).ok());
    EXPECT_FALSE(ChunkerGuc::ValidateRange("k", 201, 100, 200).ok());
}

TEST(ChunkerGucTest, LoadFromConfigOverridesDefaults) {
    InMemoryGlobalConfig cfg;
    cfg.Set(guc::kParentSize, "2048");
    cfg.Set(guc::kChildSize, "150");
    cfg.Set(guc::kChildOverlap, "0");
    cfg.Set(guc::kStrategy, "parent-child");
    cfg.Set(guc::kUnitLevel, "page");

    ChunkerConfig out;
    Status s = ChunkerGuc::LoadFromConfig(cfg, &out);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(out.parent_size, 2048);
    EXPECT_EQ(out.child_size, 150);
    EXPECT_EQ(out.child_overlap, 0);
    EXPECT_EQ(out.unit_level, UnitLevel::kPage);
}

TEST(ChunkerGucTest, LoadFromConfigKeepsDefaultsForAbsentKeys) {
    InMemoryGlobalConfig cfg;  // empty
    ChunkerConfig out;
    EXPECT_TRUE(ChunkerGuc::LoadFromConfig(cfg, &out).ok());
    EXPECT_EQ(out.parent_size, 1024);
    EXPECT_EQ(out.child_size, 200);
}

TEST(ChunkerGucTest, LoadFromConfigRejectsOutOfRange) {
    InMemoryGlobalConfig cfg;
    cfg.Set(guc::kMaxParentsPerDoc, "50");  // < 100
    ChunkerConfig out;
    EXPECT_FALSE(ChunkerGuc::LoadFromConfig(cfg, &out).ok());
}

TEST(ChunkerGucTest, LoadFromConfigNullOutRejected) {
    InMemoryGlobalConfig cfg;
    EXPECT_FALSE(ChunkerGuc::LoadFromConfig(cfg, nullptr).ok());
}

// --- ChunkerStartupValidator (child_size <= reranker.max_seq_length) ---------

TEST(ChunkerStartupValidatorTest, ValidWhenChildSizeWithinMaxSeqLength) {
    EXPECT_TRUE(ChunkerStartupValidator::ValidateChildSizeCompat(200, 512).ok());
    EXPECT_TRUE(ChunkerStartupValidator::ValidateChildSizeCompat(512, 512).ok());  // equal OK
}

TEST(ChunkerStartupValidatorTest, FailsWhenChildSizeExceedsMaxSeqLength) {
    // Case 2: child_size=600 / max_seq_length=512 -> startup fails.
    Status s = ChunkerStartupValidator::ValidateChildSizeCompat(600, 512);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_CHUNK_SIZE_INVALID"), std::string::npos);
    EXPECT_NE(s.message().find("600"), std::string::npos);
    EXPECT_NE(s.message().find("512"), std::string::npos);
}

TEST(ChunkerStartupValidatorTest, FromGlobalConfigDefaultsPass) {
    InMemoryGlobalConfig cfg;  // chunker default child_size=200, reranker default 512
    EXPECT_TRUE(ChunkerStartupValidator::ValidateStartupFromGlobalConfig(cfg).ok());
}

TEST(ChunkerStartupValidatorTest, FromGlobalConfigDetectsViolation) {
    InMemoryGlobalConfig cfg;
    cfg.Set(guc::kChildSize, "1000");
    cfg.Set("reranker.max_seq_length", "512");
    Status s = ChunkerStartupValidator::ValidateStartupFromGlobalConfig(cfg);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_CHUNK_SIZE_INVALID"), std::string::npos);
}

TEST(ChunkerStartupValidatorTest, FromGlobalConfigPassesWithRaisedMaxSeqLength) {
    InMemoryGlobalConfig cfg;
    cfg.Set(guc::kChildSize, "1000");
    cfg.Set(guc::kParentSize, "2048");  // child must also be <= parent
    cfg.Set("reranker.max_seq_length", "2048");  // raised to accommodate child
    EXPECT_TRUE(ChunkerStartupValidator::ValidateStartupFromGlobalConfig(cfg).ok());
}

TEST(ChunkerStartupValidatorTest, FromGlobalConfigRejectsBadChunkerGuc) {
    InMemoryGlobalConfig cfg;
    cfg.Set(guc::kChildOverlap, "500");  // out of range -> surfaces before compat
    Status s = ChunkerStartupValidator::ValidateStartupFromGlobalConfig(cfg);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("child_overlap"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::chunker
