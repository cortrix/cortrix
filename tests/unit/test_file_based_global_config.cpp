#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "cortrix/common/status.h"
#include "cortrix/config/file_based_global_config.h"

// S3.1 coverage: FileBasedGlobalConfig (the CE file-backed IGlobalConfig).
namespace cortrix::config {
namespace {

std::string WriteTemp(const std::string& name, const std::string& content) {
    std::string path = std::string(::testing::TempDir()) + name;
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

TEST(FileBasedGlobalConfigTest, LoadsScalarsAndTypedGetters) {
    std::string path = WriteTemp("cfg_scalars.json", R"({
        "name": "cortrix",
        "enabled": true,
        "max_rows": 100000,
        "ratio": 0.5
    })");
    FileBasedGlobalConfig cfg(path);

    auto name = cfg.GetString("name");
    ASSERT_TRUE(name.ok());
    EXPECT_EQ(name.value(), "cortrix");

    auto en = cfg.GetBool("enabled");
    ASSERT_TRUE(en.ok());
    EXPECT_TRUE(en.value());

    auto rows = cfg.GetInt("max_rows");
    ASSERT_TRUE(rows.ok());
    EXPECT_EQ(rows.value(), 100000);

    auto ratio = cfg.GetFloat("ratio");
    ASSERT_TRUE(ratio.ok());
    EXPECT_FLOAT_EQ(ratio.value(), 0.5f);

    std::remove(path.c_str());
}

TEST(FileBasedGlobalConfigTest, MissingKeyIsNotFound) {
    FileBasedGlobalConfig cfg;
    auto r = cfg.GetString("nope");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
}

TEST(FileBasedGlobalConfigTest, MalformedTypedValueIsInvalidArgument) {
    std::string path = WriteTemp("cfg_bad.json", R"({"port": "not-an-int"})");
    FileBasedGlobalConfig cfg(path);
    auto r = cfg.GetInt("port");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
    std::remove(path.c_str());
}

TEST(FileBasedGlobalConfigTest, MissingFileIsEmptyNotError) {
    FileBasedGlobalConfig cfg;
    Status st = cfg.Load(std::string(::testing::TempDir()) + "does_not_exist_xyz.json");
    EXPECT_TRUE(st.ok());  // missing file → empty config, not an error
    EXPECT_FALSE(cfg.GetString("anything").ok());
}

TEST(FileBasedGlobalConfigTest, NonObjectFileIsInvalidArgument) {
    std::string path = WriteTemp("cfg_array.json", R"([1, 2, 3])");
    FileBasedGlobalConfig cfg;
    Status st = cfg.Load(path);
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), StatusCode::kInvalidArgument);
    std::remove(path.c_str());
}

TEST(FileBasedGlobalConfigTest, CorruptJsonIsInvalidArgument) {
    std::string path = WriteTemp("cfg_corrupt.json", R"({"a": )");
    FileBasedGlobalConfig cfg;
    Status st = cfg.Load(path);
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), StatusCode::kInvalidArgument);
    std::remove(path.c_str());
}

TEST(FileBasedGlobalConfigTest, SetOverridesAndFiresOnChange) {
    FileBasedGlobalConfig cfg;
    std::string changed_key;
    cfg.OnChange([&](const std::string& k) { changed_key = k; });
    cfg.Set("feature_x", "on");
    EXPECT_EQ(changed_key, "feature_x");
    auto r = cfg.GetString("feature_x");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), "on");
}

TEST(FileBasedGlobalConfigTest, ReloadFiresOnChangeForChangedKeys) {
    std::string path = WriteTemp("cfg_reload.json", R"({"a": "1", "b": "2"})");
    FileBasedGlobalConfig cfg(path);
    int change_count = 0;
    std::string last;
    cfg.OnChange([&](const std::string& k) { ++change_count; last = k; });

    // Change b, leave a, add c.
    std::ofstream(path) << R"({"a": "1", "b": "99", "c": "3"})";
    ASSERT_TRUE(cfg.Reload().ok());
    // b changed + c added = 2 notifications (a unchanged → none).
    EXPECT_EQ(change_count, 2);
    EXPECT_EQ(cfg.GetString("b").value(), "99");
    EXPECT_EQ(cfg.GetString("c").value(), "3");
    std::remove(path.c_str());
}

TEST(FileBasedGlobalConfigTest, IsAnIGlobalConfig) {
    // Usable through the canonical interface (the whole point of §3.8).
    FileBasedGlobalConfig cfg;
    cfg.Set("k", "v");
    cortrix::IGlobalConfig& base = cfg;
    EXPECT_EQ(base.GetString("k").value(), "v");
}

}  // namespace
}  // namespace cortrix::config
