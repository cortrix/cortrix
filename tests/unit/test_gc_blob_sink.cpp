#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "cortrix/catalog/gc/blob_gc_sink.h"

// OPEN-2 Stage 3 blob sink: LocalBlobGcSink unlinks under base_dir; missing file
// is idempotent success; NullBlobGcSink is a safe no-op.
namespace cortrix::catalog::gc {
namespace {

namespace fs = std::filesystem;

class LocalBlobGcSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = fs::temp_directory_path() /
                ("gc_sink_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(base_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_, ec);
    }
    fs::path base_;
};

TEST_F(LocalBlobGcSinkTest, UnlinksExistingBlob) {
    // Create base_/ab/abcd.
    fs::create_directories(base_ / "ab");
    const fs::path blob = base_ / "ab" / "abcd";
    { std::ofstream(blob) << "data"; }
    ASSERT_TRUE(fs::exists(blob));

    LocalBlobGcSink sink(base_.string());
    auto s = sink.Unlink("ab/abcd");
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_FALSE(fs::exists(blob));
}

TEST_F(LocalBlobGcSinkTest, MissingBlobIsIdempotentSuccess) {
    LocalBlobGcSink sink(base_.string());
    auto s = sink.Unlink("zz/never_existed");
    EXPECT_TRUE(s.ok()) << "unlink of an absent blob must not be an error";
}

TEST(NullBlobGcSinkTest, AlwaysSucceeds) {
    NullBlobGcSink sink;
    EXPECT_TRUE(sink.Unlink("anything").ok());
}

}  // namespace
}  // namespace cortrix::catalog::gc
