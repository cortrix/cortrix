// TokenizerRegistry (F02 secsec 2.4-bis) branch coverage: successful load +
// register, the idempotent already-registered short-circuit, the load-failure
// pass-through, Get hit vs miss, and ResetForTest. The skeleton test already
// covers the missing-key + load-failure cases; this fills the success / idempotent
// / Get-hit branches that need a real on-disk tokenizer.json fixture.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/ml/tokenizer_registry.h"
#include "test_name_util.h"

namespace cortrix::ml {
namespace {

namespace fs = std::filesystem;

// Write a minimal BPE tokenizer.json HfTokenizer::Load() accepts.
std::string WriteMinimalTokenizerJson(const fs::path& dir) {
    nlohmann::json vocab;
    vocab["<s>"] = 0; vocab["<pad>"] = 1; vocab["</s>"] = 2; vocab["<unk>"] = 3;
    vocab["a"] = 4;
    nlohmann::json j;
    j["model"] = {{"type", "BPE"}, {"vocab", vocab}};
    std::string path = (dir / "tokenizer.json").string();
    std::ofstream f(path);
    f << j.dump();
    f.close();
    return path;
}

class TokenizerRegistryBranchTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique per test: parallel ctest processes must not share this dir
        // (a sibling's TearDown/remove_all would yank files mid-test).
        dir_ = fs::temp_directory_path() /
               (std::string("cortrix_tok_registry_test_") +
                cortrix::test::SanitizedTestName());
        fs::create_directories(dir_);
        TokenizerRegistry::ResetForTest();
    }
    void TearDown() override {
        fs::remove_all(dir_);
        TokenizerRegistry::ResetForTest();
    }
    fs::path dir_;
};

TEST_F(TokenizerRegistryBranchTest, LoadAndRegisterSuccessThenGetHit) {
    std::string path = WriteMinimalTokenizerJson(dir_);
    Status s = TokenizerRegistry::LoadAndRegister("bge-m3", path);
    ASSERT_TRUE(s.ok()) << s.message();
    // Get hit branch (it != reg.end()).
    auto tok = TokenizerRegistry::Get("bge-m3");
    ASSERT_NE(tok, nullptr);
    EXPECT_TRUE(tok->loaded());
}

TEST_F(TokenizerRegistryBranchTest, SecondRegisterIsIdempotentNoReload) {
    std::string path = WriteMinimalTokenizerJson(dir_);
    ASSERT_TRUE(TokenizerRegistry::LoadAndRegister("bge-m3", path).ok());
    auto first = TokenizerRegistry::Get("bge-m3");

    // Second call hits the "already registered + loaded" short-circuit (returns Ok
    // without constructing/loading a new tokenizer). Even pointing at a now-missing
    // path must succeed because the existing entry is reused.
    Status s2 = TokenizerRegistry::LoadAndRegister("bge-m3", "/gone/tokenizer.json");
    EXPECT_TRUE(s2.ok());
    EXPECT_EQ(TokenizerRegistry::Get("bge-m3"), first);  // same shared_ptr, not reloaded
}

TEST_F(TokenizerRegistryBranchTest, LoadFailurePassesThroughAndRegistersNothing) {
    Status s = TokenizerRegistry::LoadAndRegister("bge-m3", "/nonexistent/tokenizer.json");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(TokenizerRegistry::Get("bge-m3"), nullptr);
}

TEST_F(TokenizerRegistryBranchTest, GetMissReturnsNull) {
    EXPECT_EQ(TokenizerRegistry::Get("never-registered"), nullptr);
}

TEST_F(TokenizerRegistryBranchTest, ResetForTestClearsEntries) {
    std::string path = WriteMinimalTokenizerJson(dir_);
    ASSERT_TRUE(TokenizerRegistry::LoadAndRegister("bge-m3", path).ok());
    ASSERT_NE(TokenizerRegistry::Get("bge-m3"), nullptr);
    TokenizerRegistry::ResetForTest();
    EXPECT_EQ(TokenizerRegistry::Get("bge-m3"), nullptr);
}

}  // namespace
}  // namespace cortrix::ml
