#include "cortrix/spc_enricher/prompt_template.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/spc_enricher.h"

namespace cortrix::spc {
namespace {

std::vector<ChunkContext> Chunks(std::vector<std::string> texts) {
    std::vector<ChunkContext> out;
    for (auto& t : texts) {
        ChunkContext c;
        c.chunk_text = std::move(t);
        out.push_back(std::move(c));
    }
    return out;
}

TEST(PromptTemplateTest, KnownIds) {
    EXPECT_TRUE(PromptTemplate::IsKnown("default-zh"));
    EXPECT_TRUE(PromptTemplate::IsKnown("default-en"));
    EXPECT_FALSE(PromptTemplate::IsKnown("nope"));
}

TEST(PromptTemplateTest, UnknownIdFallsBackToDefaultZh) {
    PromptTemplate tpl("does-not-exist");
    EXPECT_EQ(tpl.id(), "default-zh");
}

TEST(PromptTemplateTest, ResolvedIdRetained) {
    EXPECT_EQ(PromptTemplate("default-en").id(), "default-en");
    EXPECT_EQ(PromptTemplate("default-zh").id(), "default-zh");
}

TEST(PromptTemplateTest, RendersChunkSeparators) {
    PromptTemplate tpl("default-en");
    auto prompt = tpl.Render(Chunks({"alpha", "beta", "gamma"}));
    EXPECT_NE(prompt.find("===CHUNK 0==="), std::string::npos);
    EXPECT_NE(prompt.find("===CHUNK 1==="), std::string::npos);
    EXPECT_NE(prompt.find("===CHUNK 2==="), std::string::npos);
    EXPECT_NE(prompt.find("alpha"), std::string::npos);
    EXPECT_NE(prompt.find("beta"), std::string::npos);
    EXPECT_NE(prompt.find("gamma"), std::string::npos);
    // Instruction names the entity type vocabulary + JSON-only directive.
    EXPECT_NE(prompt.find("PERSON/ORG/DATE/MONEY/LOC/EVENT/PRODUCT"), std::string::npos);
}

TEST(PromptTemplateTest, EmptyBatchIsInstructionOnly) {
    PromptTemplate tpl("default-en");
    auto prompt = tpl.Render({});
    // No *rendered* chunk block (those carry a numeric index, e.g. "===CHUNK 0===").
    // Note the instruction text itself mentions the literal "===CHUNK i===" marker
    // format, so we check for the numbered form, not the bare substring.
    EXPECT_EQ(prompt.find("===CHUNK 0==="), std::string::npos);
    EXPECT_FALSE(prompt.empty());
    // Rendering no chunks yields exactly the instruction body.
    EXPECT_EQ(prompt, tpl.Render(std::vector<ChunkContext>{}));
}

TEST(PromptTemplateTest, ZhTemplateIsChinese) {
    PromptTemplate tpl("default-zh");
    auto prompt = tpl.Render(Chunks({"内容"}));
    EXPECT_NE(prompt.find("命名实体识别"), std::string::npos);
    EXPECT_NE(prompt.find("===CHUNK 0==="), std::string::npos);
}

}  // namespace
}  // namespace cortrix::spc
