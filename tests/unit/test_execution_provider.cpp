#include <gtest/gtest.h>

#include <string>

#include "cortrix/ml/execution_provider.h"

namespace cortrix::ml {
namespace {

TEST(ExecutionProviderTest, ParseExecutionProviderIsCaseInsensitive) {
    ExecutionProvider parsed = ExecutionProvider::kAuto;

    EXPECT_TRUE(ParseExecutionProvider("auto", &parsed).ok());
    EXPECT_EQ(parsed, ExecutionProvider::kAuto);

    EXPECT_TRUE(ParseExecutionProvider("CPU", &parsed).ok());
    EXPECT_EQ(parsed, ExecutionProvider::kCpu);

    EXPECT_TRUE(ParseExecutionProvider("CoreML", &parsed).ok());
    EXPECT_EQ(parsed, ExecutionProvider::kCoreMl);

    EXPECT_TRUE(ParseExecutionProvider("cUdA", &parsed).ok());
    EXPECT_EQ(parsed, ExecutionProvider::kCuda);
}

TEST(ExecutionProviderTest, ParseExecutionProviderRejectsInvalidAndNullOutput) {
    ExecutionProvider parsed = ExecutionProvider::kAuto;
    EXPECT_FALSE(ParseExecutionProvider("metal", &parsed).ok());
    EXPECT_TRUE(ParseExecutionProvider("tpu", &parsed).message().find("must be one of") !=
                std::string::npos);
    EXPECT_FALSE(ParseExecutionProvider("auto", nullptr).ok());
}

TEST(ExecutionProviderTest, ExecutionProviderNameRoundTrip) {
    EXPECT_STREQ(ExecutionProviderName(ExecutionProvider::kAuto), "auto");
    EXPECT_STREQ(ExecutionProviderName(ExecutionProvider::kCpu), "cpu");
    EXPECT_STREQ(ExecutionProviderName(ExecutionProvider::kCoreMl), "coreml");
    EXPECT_STREQ(ExecutionProviderName(ExecutionProvider::kCuda), "cuda");
}

TEST(ExecutionProviderTest, ValidateExecutionProviderForBuildMatchesCompileFlags) {
    EXPECT_TRUE(ValidateExecutionProviderForBuild(ExecutionProvider::kAuto).ok());
    EXPECT_TRUE(ValidateExecutionProviderForBuild(ExecutionProvider::kCpu).ok());

    auto coreml = ValidateExecutionProviderForBuild(ExecutionProvider::kCoreMl);
#ifdef CORTRIX_HAS_COREML
    EXPECT_TRUE(coreml.ok());
#else
    EXPECT_FALSE(coreml.ok());
    EXPECT_NE(coreml.message().find("not compiled into this Cortrix build"),
              std::string::npos);
#endif

    auto cuda = ValidateExecutionProviderForBuild(ExecutionProvider::kCuda);
#ifdef CORTRIX_HAS_CUDA
    EXPECT_TRUE(cuda.ok());
#else
    EXPECT_FALSE(cuda.ok());
    EXPECT_NE(cuda.message().find("not compiled into this Cortrix build"),
              std::string::npos);
#endif
}

TEST(ExecutionProviderTest, PreferredAutoExecutionProviderDependsOnBuildCapability) {
#ifdef CORTRIX_HAS_CUDA
    EXPECT_EQ(PreferredAutoExecutionProvider(), ExecutionProvider::kCuda);
#elif defined(CORTRIX_HAS_COREML)
    EXPECT_EQ(PreferredAutoExecutionProvider(), ExecutionProvider::kCoreMl);
#else
    EXPECT_EQ(PreferredAutoExecutionProvider(), ExecutionProvider::kCpu);
#endif
}

}  // namespace
}  // namespace cortrix::ml
