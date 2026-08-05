#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/onnx/onnx_error.h"
#include "cortrix/onnx/runtime.h"
#include "cortrix/onnx/startup_validator.h"

namespace cortrix::onnx {
namespace {

// ---------------------------------------------------------------------------
// Minimal ONNX ModelProto fixture builder.
//
// We synthesize just enough of the protobuf wire format for StartupValidator::
// ReadModelOpset to find an opset — the ModelProto.opset_import field (field 8,
// repeated OperatorSetIdProto), each entry carrying domain (field 1, string)
// and version (field 2, varint). This mirrors what a real .onnx header carries
// and lets us cover arbitrary opsets (incl. the future-opset abort case)
// without checking in binary model blobs.
// ---------------------------------------------------------------------------

void AppendVarint(std::vector<uint8_t>* out, uint64_t v) {
    while (v >= 0x80) {
        out->push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out->push_back(static_cast<uint8_t>(v));
}

void AppendTag(std::vector<uint8_t>* out, uint32_t field, uint32_t wire_type) {
    AppendVarint(out, (static_cast<uint64_t>(field) << 3) | wire_type);
}

// One OperatorSetIdProto { domain, version } encoded as a LEN-delimited message.
std::vector<uint8_t> EncodeOpsetEntry(const std::string& domain, int64_t version) {
    std::vector<uint8_t> inner;
    if (!domain.empty()) {
        AppendTag(&inner, 1, 2);  // domain (string)
        AppendVarint(&inner, domain.size());
        inner.insert(inner.end(), domain.begin(), domain.end());
    }
    AppendTag(&inner, 2, 0);      // version (varint)
    AppendVarint(&inner, static_cast<uint64_t>(version));
    return inner;
}

// A ModelProto carrying one opset_import {domain,version} + a decoy ir_version
// field (field 1, varint) so we also exercise the top-level SkipField path.
std::vector<uint8_t> BuildModelProto(int64_t opset, const std::string& domain = "") {
    std::vector<uint8_t> out;
    // ir_version = field 1, varint (a field we must skip while scanning).
    AppendTag(&out, 1, 0);
    AppendVarint(&out, 9);
    // opset_import = field 8, LEN message.
    std::vector<uint8_t> entry = EncodeOpsetEntry(domain, opset);
    AppendTag(&out, 8, 2);
    AppendVarint(&out, entry.size());
    out.insert(out.end(), entry.begin(), entry.end());
    return out;
}

// Write bytes to a unique temp .onnx file; returns its path.
std::string WriteTempModel(const std::string& name, const std::vector<uint8_t>& bytes) {
    std::string path =
        (std::filesystem::temp_directory_path() / ("cortrix_f22_" + name + ".onnx")).string();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.close();
    return path;
}

// ============================================================
// ReadModelOpset — protobuf header parse
// ============================================================

TEST(OnnxOpsetReaderTest, ReadsDefaultDomainOpset) {
    std::string path = WriteTempModel("opset17", BuildModelProto(17));
    auto r = StartupValidator::ReadModelOpset(path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 17);
    std::filesystem::remove(path);
}

TEST(OnnxOpsetReaderTest, ReadsExplicitAiOnnxDomain) {
    std::string path = WriteTempModel("aionnx21", BuildModelProto(21, "ai.onnx"));
    auto r = StartupValidator::ReadModelOpset(path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 21);
    std::filesystem::remove(path);
}

TEST(OnnxOpsetReaderTest, MissingFileFails) {
    auto r = StartupValidator::ReadModelOpset("/no/such/model.onnx");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_ONNX_OPSET_INCOMPATIBLE"), std::string::npos);
}

TEST(OnnxOpsetReaderTest, EmptyFileFails) {
    std::string path = WriteTempModel("empty", {});
    auto r = StartupValidator::ReadModelOpset(path);
    EXPECT_FALSE(r.ok());
    std::filesystem::remove(path);
}

TEST(OnnxOpsetReaderTest, NoOpsetImportFails) {
    // ModelProto with only ir_version, no opset_import → unreadable opset.
    std::vector<uint8_t> bytes;
    AppendTag(&bytes, 1, 0);
    AppendVarint(&bytes, 9);
    std::string path = WriteTempModel("noopset", bytes);
    auto r = StartupValidator::ReadModelOpset(path);
    EXPECT_FALSE(r.ok());
    std::filesystem::remove(path);
}

// ============================================================
// Validate — runtime version check
// ============================================================

TEST(OnnxStartupValidatorTest, EmptyModelListPassesOnCleanBuild) {
    // No models + a clean build (runtime major == compiled major) → OK.
    StartupValidator::ValidationConfig cfg;  // empty registered_model_paths
    Status s = StartupValidator::Validate(cfg);
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST(OnnxStartupValidatorTest, ReportExposesRuntimeFacts) {
    StartupValidator::ValidationConfig cfg;
    auto report = StartupValidator::ValidateVerbose(cfg);
    EXPECT_TRUE(report.status.ok());
    if (Runtime::IsRuntimeLinked()) {
        EXPECT_FALSE(report.runtime_version.empty());
        EXPECT_GT(report.runtime_major, 0);
        EXPECT_LE(report.supported_opset_range.first, report.supported_opset_range.second);
    }
}

// ============================================================
// Validate — opset compatibility (TC-1..TC-9 family)
// ============================================================

class OnnxValidateOpsetTest : public ::testing::Test {
 protected:
    void SetUp() override {
        if (!Runtime::IsRuntimeLinked()) {
            GTEST_SKIP() << "ONNX Runtime not linked; opset-range checks need a live runtime";
        }
        auto range = Runtime::GetSupportedOpsetRange();
        min_opset_ = range.first;
        max_opset_ = range.second;
    }
    void TearDown() override {
        for (const auto& p : temp_paths_) std::filesystem::remove(p);
    }
    std::string MakeModel(const std::string& name, int opset) {
        std::string path = WriteTempModel(name, BuildModelProto(opset));
        temp_paths_.push_back(path);
        return path;
    }
    int min_opset_ = 0;
    int max_opset_ = 0;
    std::vector<std::string> temp_paths_;
};

// TC-1 / TC-2: a single in-range model (mid-range) validates.
TEST_F(OnnxValidateOpsetTest, InRangeModelPasses) {
    int mid = (min_opset_ + max_opset_) / 2;
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {MakeModel("inrange", mid)};
    EXPECT_TRUE(StartupValidator::Validate(cfg).ok());
}

// TC-7: lower boundary (== min_opset) passes.
TEST_F(OnnxValidateOpsetTest, LowerBoundaryPasses) {
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {MakeModel("lower", min_opset_)};
    EXPECT_TRUE(StartupValidator::Validate(cfg).ok());
}

// TC-8: upper boundary (== max_opset) passes.
TEST_F(OnnxValidateOpsetTest, UpperBoundaryPasses) {
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {MakeModel("upper", max_opset_)};
    EXPECT_TRUE(StartupValidator::Validate(cfg).ok());
}

// TC-9: just above upper boundary fails with the opset error code.
TEST_F(OnnxValidateOpsetTest, AboveUpperBoundaryFails) {
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {MakeModel("above", max_opset_ + 1)};
    Status s = StartupValidator::Validate(cfg);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_ONNX_OPSET_INCOMPATIBLE"), std::string::npos);
}

// TC-4 family: a far-future opset (99) aborts.
TEST_F(OnnxValidateOpsetTest, FutureOpset99Fails) {
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {MakeModel("future99", 99)};
    EXPECT_FALSE(StartupValidator::Validate(cfg).ok());
}

// Below the floor (opset < min) also fails.
TEST_F(OnnxValidateOpsetTest, BelowFloorFails) {
    if (min_opset_ <= 1) GTEST_SKIP() << "floor too low to test below-floor";
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {MakeModel("belowfloor", min_opset_ - 1)};
    EXPECT_FALSE(StartupValidator::Validate(cfg).ok());
}

// TC-3: two in-range models (reranker + OnnxEmbedder shape) both validate.
TEST_F(OnnxValidateOpsetTest, TwoModelsBothInRangePass) {
    int mid = (min_opset_ + max_opset_) / 2;
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {MakeModel("reranker", mid), MakeModel("embedder", mid)};
    auto report = StartupValidator::ValidateVerbose(cfg);
    EXPECT_TRUE(report.status.ok());
    ASSERT_EQ(report.models.size(), 2u);
    EXPECT_TRUE(report.models[0].checked);
    EXPECT_TRUE(report.models[1].checked);
}

// First model OK, second out-of-range → fails on the second (and stops there).
TEST_F(OnnxValidateOpsetTest, SecondModelOutOfRangeFails) {
    int mid = (min_opset_ + max_opset_) / 2;
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {MakeModel("ok", mid), MakeModel("bad", 99)};
    EXPECT_FALSE(StartupValidator::Validate(cfg).ok());
}

// ============================================================
// Validate — missing model policy
// ============================================================

TEST(OnnxStartupValidatorTest, MissingModelHardFailsByDefault) {
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {"/no/such/registered.onnx"};
    // Default skip_missing_models=false → a registered-but-absent model aborts.
    if (!Runtime::IsRuntimeLinked()) {
        GTEST_SKIP() << "no runtime linked → validation short-circuits before model loop";
    }
    Status s = StartupValidator::Validate(cfg);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("not found"), std::string::npos);
}

TEST(OnnxStartupValidatorTest, MissingModelSkippedWhenConfigured) {
    StartupValidator::ValidationConfig cfg;
    cfg.registered_model_paths = {"/no/such/registered.onnx"};
    cfg.skip_missing_models = true;
    auto report = StartupValidator::ValidateVerbose(cfg);
    EXPECT_TRUE(report.status.ok());
    if (Runtime::IsRuntimeLinked()) {
        ASSERT_EQ(report.models.size(), 1u);
        EXPECT_FALSE(report.models[0].checked);  // skipped, not validated
    }
}

// ============================================================
// CollectRegisteredOnnxModels — config → ValidationConfig (standalone scope)
// ============================================================

// CollectRegisteredOnnxModels also conditionally registers the query routing
// query-complexity model (config.query_complexity.model_dir) and the reranker
// reranker model (config.reranker.model_dir) when their model.onnx exists.
// Pin both dirs to nonexistent paths via config (NOT env — the collector now
// resolves them from config only) so these exact-set assertions hold
// regardless of where the suite runs (repo root has the real models;
// separate source checkouts do not).
class OnnxCollectModelsTest : public ::testing::Test {
 protected:
    CortrixConfig BaseConfig() {
        CortrixConfig config;
        config.query_complexity.model_dir = "/nonexistent-query-complexity";
        config.reranker.model_dir = "/nonexistent-reranker";
        return config;
    }
};

TEST_F(OnnxCollectModelsTest, RegistersEmbedderModelPath) {
    CortrixConfig config = BaseConfig();
    config.embedding.model_path = "/models/bge-m3/model.onnx";
    auto cfg = StartupValidator::CollectRegisteredOnnxModels(config);
    ASSERT_EQ(cfg.registered_model_paths.size(), 1u);
    EXPECT_EQ(cfg.registered_model_paths[0], "/models/bge-m3/model.onnx");
}

TEST_F(OnnxCollectModelsTest, EmptyModelPathRegistersNothing) {
    // Stub-only deployment: no model_path → nothing registered (validation
    // becomes a no-op pass).
    CortrixConfig config = BaseConfig();  // embedding.model_path defaults empty
    auto cfg = StartupValidator::CollectRegisteredOnnxModels(config);
    EXPECT_TRUE(cfg.registered_model_paths.empty());
}

// ============================================================
// Sparse retrieval S2 — ONNX Runtime reuse closure
// ============================================================
// BGE-M3 produces dense + sparse from a SINGLE model in one forward pass, so
// Sparse path reuses the very BGE-M3 model the OnnxEmbedder already loads
// (config.embedding.model_path) — there is NO separate sparse model to
// register. These tests pin that closure: the single registration validated for
// the dense path also certifies the model sparse path runs on. Appending
// any *distinct* future model to the collector stays D3.5 (collector comment).

TEST_F(OnnxCollectModelsTest, F40SparseReusesBgeM3SingleRegistration) {
    // The bge-m3 registration is shared: registering it once covers both the
    // dense embedder and the sparse activation (single model, single path).
    CortrixConfig config = BaseConfig();
    config.embedding.model_path = "/models/bge-m3/model.onnx";
    auto cfg = StartupValidator::CollectRegisteredOnnxModels(config);
    ASSERT_EQ(cfg.registered_model_paths.size(), 1u)
        << "F40 must NOT add a 2nd model path — it reuses bge-m3";
    EXPECT_EQ(cfg.registered_model_paths[0], "/models/bge-m3/model.onnx");
}

TEST_F(OnnxCollectModelsTest, F40SparseStubOnlyRegistersNothing) {
    // Stub-only (no model) deployment: sparse runs on the deterministic stub
    // too, so — like the dense path — there is nothing to validate at startup.
    CortrixConfig config = BaseConfig();  // empty embedding.model_path
    auto cfg = StartupValidator::CollectRegisteredOnnxModels(config);
    EXPECT_TRUE(cfg.registered_model_paths.empty());
}

}  // namespace
}  // namespace cortrix::onnx
