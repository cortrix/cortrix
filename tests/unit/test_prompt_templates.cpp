#include <gtest/gtest.h>

#include <string>

#include "cortrix/memory/prompt_templates.h"

// S1 coverage: the memory extraction prompt template constants + the render/CJK helpers.
namespace cortrix::memory {
namespace {

TEST(PromptTemplatesTest, ExtractTemplatesAreNonEmptyAndDistinct) {
    EXPECT_GT(std::string(kMemoryExtractPromptZh).size(), 0u);
    EXPECT_GT(std::string(kMemoryExtractPromptEn).size(), 0u);
    EXPECT_NE(std::string(kMemoryExtractPromptZh), std::string(kMemoryExtractPromptEn));
}

TEST(PromptTemplatesTest, ContradictionTemplatesAreNonEmptyAndDistinct) {
    EXPECT_GT(std::string(kContradictionDetectPromptZh).size(), 0u);
    EXPECT_GT(std::string(kContradictionDetectPromptEn).size(), 0u);
    EXPECT_NE(std::string(kContradictionDetectPromptZh),
              std::string(kContradictionDetectPromptEn));
}

TEST(PromptTemplatesTest, ExtractTemplatesCarryTheWindowPlaceholder) {
    EXPECT_NE(std::string(kMemoryExtractPromptZh).find(kPlaceholderInteractionWindow),
              std::string::npos);
    EXPECT_NE(std::string(kMemoryExtractPromptEn).find(kPlaceholderInteractionWindow),
              std::string::npos);
}

TEST(PromptTemplatesTest, ContradictionTemplatesCarryBothFactPlaceholders) {
    for (const char* t : {kContradictionDetectPromptZh, kContradictionDetectPromptEn}) {
        std::string s(t);
        EXPECT_NE(s.find(kPlaceholderNewFact), std::string::npos);
        EXPECT_NE(s.find(kPlaceholderOldFact), std::string::npos);
    }
}

TEST(PromptTemplatesTest, ContainsCjkDetectsChinese) {
    EXPECT_TRUE(ContainsCjk("用户在上海"));
    EXPECT_TRUE(ContainsCjk("hello 世界"));
    EXPECT_FALSE(ContainsCjk("user is in Shanghai"));
    EXPECT_FALSE(ContainsCjk(""));
    // Emoji (4-byte UTF-8) is not CJK.
    EXPECT_FALSE(ContainsCjk("nice \xF0\x9F\x98\x80"));
    // 2-byte UTF-8 (Latin-1 supplement, e.g. é = 0xC3 0xA9) is not CJK.
    EXPECT_FALSE(ContainsCjk("caf\xC3\xA9"));
    // A bare/malformed continuation lead byte (0x80) must not crash + not match.
    EXPECT_FALSE(ContainsCjk("\x80\x81 plain"));
    // 3-byte non-CJK (e.g. U+20AC EURO SIGN = 0xE2 0x82 0xAC) is not CJK.
    EXPECT_FALSE(ContainsCjk("price \xE2\x82\xAC"));
}

TEST(PromptTemplatesTest, AutoSelectsLanguageByCjk) {
    EXPECT_EQ(ExtractPromptTemplate(PromptLang::kAuto, "我喜欢深色主题"),
              kMemoryExtractPromptZh);
    EXPECT_EQ(ExtractPromptTemplate(PromptLang::kAuto, "I prefer dark theme"),
              kMemoryExtractPromptEn);
    EXPECT_EQ(ContradictionPromptTemplate(PromptLang::kAuto, "用户在北京"),
              kContradictionDetectPromptZh);
    EXPECT_EQ(ContradictionPromptTemplate(PromptLang::kAuto, "user in Beijing"),
              kContradictionDetectPromptEn);
}

TEST(PromptTemplatesTest, ExplicitLangOverridesHeuristic) {
    // Force EN even though the sample is Chinese, and vice versa.
    EXPECT_EQ(ExtractPromptTemplate(PromptLang::kEn, "用户在上海"), kMemoryExtractPromptEn);
    EXPECT_EQ(ExtractPromptTemplate(PromptLang::kZh, "english text"), kMemoryExtractPromptZh);
    // Same for the contradiction template's explicit selectors.
    EXPECT_EQ(ContradictionPromptTemplate(PromptLang::kZh, "english"),
              kContradictionDetectPromptZh);
    EXPECT_EQ(ContradictionPromptTemplate(PromptLang::kEn, "中文"),
              kContradictionDetectPromptEn);
}

TEST(PromptTemplatesTest, RenderPromptSubstitutesAllOccurrences) {
    std::string tmpl = "A {x} B {x} C";
    EXPECT_EQ(RenderPrompt(tmpl, "{x}", "Z"), "A Z B Z C");
}

TEST(PromptTemplatesTest, RenderPromptNoMatchReturnsTemplate) {
    std::string tmpl = "no placeholder here";
    EXPECT_EQ(RenderPrompt(tmpl, "{x}", "Z"), tmpl);
}

TEST(PromptTemplatesTest, RenderPromptEmptyTokenIsNoOp) {
    std::string tmpl = "unchanged {x}";
    EXPECT_EQ(RenderPrompt(tmpl, "", "Z"), tmpl);
}

TEST(PromptTemplatesTest, RenderPromptFillsWindowInRealTemplate) {
    std::string filled =
        RenderPrompt(kMemoryExtractPromptEn, kPlaceholderInteractionWindow,
                     "user: I just moved to Shanghai");
    EXPECT_EQ(filled.find(kPlaceholderInteractionWindow), std::string::npos);
    EXPECT_NE(filled.find("I just moved to Shanghai"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::memory
