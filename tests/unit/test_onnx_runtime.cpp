#include <gtest/gtest.h>

#include "cortrix/onnx/runtime.h"

namespace cortrix::onnx {
namespace {

// ============================================================
// Compiled major version (CMake-locked CORTRIX_ONNXRT_EXPECTED_MAJOR)
// ============================================================

TEST(OnnxRuntimeTest, CompiledMajorVersionIsString) {
    // F22 §8.3: expected_major_version is a STRING ("1"), matching the CMake
    // flag ONNXRT_MAJOR_VERSION. Default build locks major 1.
    EXPECT_EQ(Runtime::GetCompiledMajorVersion(), "1");
}

// ============================================================
// Runtime linkage + version introspection
// ============================================================

TEST(OnnxRuntimeTest, RuntimeVersionConsistentWithLinkage) {
    // When the binary is built with ONNX Runtime linked (CORTRIX_USE_ONNX=ON),
    // GetRuntimeVersion() returns the loaded runtime's version and the major is
    // > 0. When unlinked, both report their "absent" sentinels. Either way the
    // two getters must agree on whether a runtime is present.
    const bool linked = Runtime::IsRuntimeLinked();
    const std::string version = Runtime::GetRuntimeVersion();
    const int major = Runtime::GetRuntimeMajorVersion();

    if (linked) {
        EXPECT_FALSE(version.empty());
        EXPECT_GT(major, 0);
        // The shipped runtime (Dependencies.cmake) is 1.17.1 → major 1.
        EXPECT_EQ(major, 1);
    } else {
        EXPECT_TRUE(version.empty());
        EXPECT_EQ(major, 0);
    }
}

TEST(OnnxRuntimeTest, RuntimeMajorMatchesCompiledMajorWhenLinked) {
    // The default build links the 1.x runtime and is compiled for major "1", so
    // they match — i.e. a clean build never trips CX_ERR_ONNXRT_VERSION_MISMATCH.
    if (!Runtime::IsRuntimeLinked()) {
        GTEST_SKIP() << "ONNX Runtime not linked in this build";
    }
    EXPECT_EQ(std::to_string(Runtime::GetRuntimeMajorVersion()),
              Runtime::GetCompiledMajorVersion());
}

// ============================================================
// Supported opset range
// ============================================================

TEST(OnnxRuntimeTest, SupportedOpsetRangeWhenLinked) {
    if (!Runtime::IsRuntimeLinked()) {
        GTEST_SKIP() << "ONNX Runtime not linked in this build";
    }
    auto [lo, hi] = Runtime::GetSupportedOpsetRange();
    // Major-1 runtime: floor is opset 7, and the upper bound is a sane,
    // ordered value (1.17 → 19 per the ORT support matrix).
    EXPECT_EQ(lo, 7);
    EXPECT_GE(hi, 19);
    EXPECT_LE(lo, hi);
}

TEST(OnnxRuntimeTest, SupportedOpsetRangeAbsentWhenUnlinked) {
    if (Runtime::IsRuntimeLinked()) {
        GTEST_SKIP() << "ONNX Runtime linked — absent-sentinel path not exercised";
    }
    auto [lo, hi] = Runtime::GetSupportedOpsetRange();
    EXPECT_EQ(lo, 0);
    EXPECT_EQ(hi, 0);
}

}  // namespace
}  // namespace cortrix::onnx
