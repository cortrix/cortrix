// Coverage-gap unit tests for src/config/file_based_global_config.cpp (74.5%).
//
// The existing test_file_based_global_config.cpp covers the happy path + a single
// GetInt-parse failure + Set/Reload OnChange. This file targets the remaining
// typed-getter error branches that the spec leaves uncovered:
//
//   GetBool   — value that is neither true/1 nor false/0  -> kInvalidArgument
//   GetInt    — trailing garbage after digits             -> kInvalidArgument
//   GetInt    — overflow (out_of_range from std::stoi)    -> catch -> kInvalidArgument
//   GetFloat  — trailing garbage after the number         -> kInvalidArgument
//   GetFloat  — overflow (out_of_range from std::stof)    -> catch -> kInvalidArgument
//   OnChange  — Reload removed-key notification path
//
// All branches are driven through Set() (pure in-memory, no file I/O needed) so
// the tests are fully deterministic. Suite name is module-prefixed + unique.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/config/file_based_global_config.h"

namespace cortrix::config {
namespace {

// GetBool on a value that is not a recognized bool token -> InvalidArgument.
TEST(FileConfigGapTest, GetBoolNonBoolIsInvalidArgument) {
    FileBasedGlobalConfig cfg;
    cfg.Set("flag", "maybe");
    Result<bool> r = cfg.GetBool("flag");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
}

// Accepted bool tokens still parse (covers the true/1 and false/0 success arms).
TEST(FileConfigGapTest, GetBoolAcceptsOneAndZero) {
    FileBasedGlobalConfig cfg;
    cfg.Set("a", "1");
    cfg.Set("b", "0");
    Result<bool> ra = cfg.GetBool("a");
    Result<bool> rb = cfg.GetBool("b");
    ASSERT_TRUE(ra.ok());
    ASSERT_TRUE(rb.ok());
    EXPECT_TRUE(ra.value());
    EXPECT_FALSE(rb.value());
}

// GetInt with digits followed by trailing garbage -> consumed != size -> error.
TEST(FileConfigGapTest, GetIntTrailingGarbageIsInvalidArgument) {
    FileBasedGlobalConfig cfg;
    cfg.Set("n", "123abc");
    Result<int> r = cfg.GetInt("n");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
}

// GetInt overflow -> std::stoi throws out_of_range -> catch -> InvalidArgument.
TEST(FileConfigGapTest, GetIntOverflowIsInvalidArgument) {
    FileBasedGlobalConfig cfg;
    cfg.Set("big", "99999999999999999999");  // far beyond INT_MAX
    Result<int> r = cfg.GetInt("big");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
}

// GetFloat with trailing garbage after a valid number -> consumed != size.
TEST(FileConfigGapTest, GetFloatTrailingGarbageIsInvalidArgument) {
    FileBasedGlobalConfig cfg;
    cfg.Set("f", "1.5xyz");
    Result<float> r = cfg.GetFloat("f");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
}

// GetFloat overflow -> std::stof throws out_of_range -> catch -> InvalidArgument.
TEST(FileConfigGapTest, GetFloatOverflowIsInvalidArgument) {
    FileBasedGlobalConfig cfg;
    cfg.Set("huge", "1e400");  // beyond float (and double) range
    Result<float> r = cfg.GetFloat("huge");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
}

// Reload that drops a previously-present key must fire OnChange for the removed
// key (the second NotifyLocked loop in Load()).
TEST(FileConfigGapTest, ReloadFiresOnChangeForRemovedKey) {
    std::string path = std::string(::testing::TempDir()) +
                       "cfg_gap_removed_" + std::to_string(::getpid()) + ".json";
    { std::ofstream(path) << R"({"keep": "1", "gone": "2"})"; }

    FileBasedGlobalConfig cfg(path);
    ASSERT_TRUE(cfg.GetString("gone").ok());

    std::vector<std::string> changed;
    cfg.OnChange([&](const std::string& k) { changed.push_back(k); });

    // Rewrite the file without "gone": Reload must notify that key as removed.
    { std::ofstream(path) << R"({"keep": "1"})"; }
    ASSERT_TRUE(cfg.Reload().ok());

    bool saw_gone = false;
    for (const auto& k : changed) {
        if (k == "gone") saw_gone = true;
    }
    EXPECT_TRUE(saw_gone);
    EXPECT_FALSE(cfg.GetString("gone").ok());  // removed from the map
    std::remove(path.c_str());
}

// Reload with an empty (never-loaded) path is a no-op Ok (early return arm).
TEST(FileConfigGapTest, ReloadWithoutPathIsOk) {
    FileBasedGlobalConfig cfg;  // default ctor: path_ stays empty
    Status s = cfg.Reload();
    EXPECT_TRUE(s.ok());
}

}  // namespace
}  // namespace cortrix::config
