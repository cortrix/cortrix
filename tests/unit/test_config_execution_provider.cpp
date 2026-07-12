#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "cortrix/config/config.h"

namespace cortrix {
namespace {

namespace {
bool ContainsError(const std::vector<std::string>& errors, const std::string& needle) {
    for (const auto& e : errors) {
        if (e.find(needle) != std::string::npos) return true;
    }
    return false;
}
}  // namespace

class ConfigExecutionProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_provider_config_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        unsetenv("CORTRIX_RERANKER_EXECUTION_PROVIDER");
        unsetenv("CORTRIX_RERANKER_USE_COREML");
        unsetenv("CORTRIX_EMBEDDING_EXECUTION_PROVIDER");
        unsetenv("CORTRIX_EMBEDDING_DEVICE");
    }

    void TearDown() override {
        system(("rm -rf " + tmp_dir_).c_str());
        unsetenv("CORTRIX_RERANKER_EXECUTION_PROVIDER");
        unsetenv("CORTRIX_RERANKER_USE_COREML");
        unsetenv("CORTRIX_EMBEDDING_EXECUTION_PROVIDER");
        unsetenv("CORTRIX_EMBEDDING_DEVICE");
    }

    void WriteYaml(const std::string& content) {
        yaml_path_ = tmp_dir_ + "/provider.yaml";
        std::ofstream f(yaml_path_);
        f << content;
        f.close();
    }

    std::string tmp_dir_;
    std::string yaml_path_;
};

TEST_F(ConfigExecutionProviderTest, RerankerCanonicalWinsLegacyYaml) {
    WriteYaml(R"(
reranker:
  execution_provider: "coreml"
  use_coreml: false
)");
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.reranker.execution_provider, "coreml");
    EXPECT_TRUE(config.reranker.execution_provider_error.empty());
}

TEST_F(ConfigExecutionProviderTest, RerankerLegacyConflictIsDetected) {
    WriteYaml(R"(
reranker:
  execution_provider: "cpu"
  use_coreml: true
)");
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.reranker.execution_provider, "cpu");
    EXPECT_EQ(config.reranker.execution_provider_error,
              "reranker canonical execution_provider conflicts with deprecated alias");
    auto errors = ValidateConfig(config);
    EXPECT_TRUE(ContainsError(errors, "reranker canonical execution_provider conflicts with deprecated "
                                       "alias"));
}

TEST_F(ConfigExecutionProviderTest, RerankerEnvProviderOverridesYamlAndKeepsPrecedence) {
    WriteYaml(R"(
reranker:
  execution_provider: "cpu"
)");
    setenv("CORTRIX_RERANKER_EXECUTION_PROVIDER", "coreml", 1);
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.reranker.execution_provider, "coreml");
}

TEST_F(ConfigExecutionProviderTest, RerankerCanonicalEnvClearsYamlConflict) {
    WriteYaml(R"(
reranker:
  execution_provider: "cpu"
  use_coreml: true
)");
    setenv("CORTRIX_RERANKER_EXECUTION_PROVIDER", "cuda", 1);
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.reranker.execution_provider, "cuda");
    EXPECT_TRUE(config.reranker.execution_provider_error.empty());
}

TEST_F(ConfigExecutionProviderTest, EmbeddingCanonicalWinsLegacyAlias) {
    WriteYaml(R"(
embedding:
  execution_provider: "coreml"
  gpu_provider: "CoreML"
)");
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.embedding.execution_provider, "coreml");
    EXPECT_TRUE(config.embedding.execution_provider_error.empty());
}

TEST_F(ConfigExecutionProviderTest, EmbeddingLegacyConflictIsDetected) {
    WriteYaml(R"(
embedding:
  execution_provider: "coreml"
  gpu_provider: "cuda"
)");
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.embedding.execution_provider_error,
              "embedding canonical execution_provider conflicts with deprecated alias");
    auto errors = ValidateConfig(config);
    EXPECT_TRUE(ContainsError(errors, "embedding canonical execution_provider conflicts with deprecated "
                                       "alias"));
}

TEST_F(ConfigExecutionProviderTest, EmbeddingCanonicalEnvClearsYamlConflict) {
    WriteYaml(R"(
embedding:
  execution_provider: "coreml"
  gpu_provider: "cpu"
)");
    setenv("CORTRIX_EMBEDDING_EXECUTION_PROVIDER", "cuda", 1);
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.embedding.execution_provider, "cuda");
    EXPECT_TRUE(config.embedding.execution_provider_error.empty());
}

TEST(ConfigEmbeddingCompatibilityTest, LegacyAggregateFieldStillCompiles) {
    EmbeddingConfig config{"model.onnx", "tokenizer.json", 1024, 512, "cpu"};
    EXPECT_EQ(config.gpu_provider, "cpu");
    EXPECT_EQ(config.execution_provider, "auto");
}

TEST_F(ConfigExecutionProviderTest, LegacyUseCoremlFalseDoesNotOverrideAuto) {
    setenv("CORTRIX_RERANKER_USE_COREML", "0", 1);
    auto config = LoadConfig("");
    EXPECT_EQ(config.reranker.execution_provider, "auto");
    EXPECT_TRUE(config.reranker.execution_provider_error.empty());
}

TEST_F(ConfigExecutionProviderTest, ExplicitCudaBuildValidation) {
    CortrixConfig config;
    config.embedding.execution_provider = "cuda";
    config.reranker.execution_provider = "cuda";

    auto errors = ValidateConfig(config);
#ifdef CORTRIX_HAS_CUDA
    EXPECT_FALSE(ContainsError(errors, "embedding.execution_provider: execution provider 'cuda' is not "
                                      "compiled into this Cortrix build"));
    EXPECT_FALSE(ContainsError(errors, "reranker.execution_provider: execution provider 'cuda' is not "
                                      "compiled into this Cortrix build"));
#else
    EXPECT_TRUE(ContainsError(errors, "embedding.execution_provider: execution provider 'cuda' is not "
                                     "compiled into this Cortrix build"));
    EXPECT_TRUE(ContainsError(errors, "reranker.execution_provider: execution provider 'cuda' is not "
                                     "compiled into this Cortrix build"));
#endif
}

}  // namespace
}  // namespace cortrix
