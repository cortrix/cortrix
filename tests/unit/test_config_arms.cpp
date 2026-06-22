// Coverage-closure (CLOSURE90) unit tests for src/config/config.cpp and
// src/config/file_based_global_config.cpp -- the YAML-parse sections and
// validation arms that the existing test_config.cpp / test_config_coverage_gaps.cpp
// suites leave uncovered, plus the FileBasedGlobalConfig null/object coercion and
// SetAgentLlmConfig round-trip arms.
//
// Targeted uncovered lines (gcov DA=0):
//   config.cpp 89                  -- namespace.load_timeout_ms_per_ns parse
//   config.cpp 216-219             -- memory.decay.{lambda,min_score} parse
//   config.cpp 224-227             -- security.{admin_bind,allow_public_admin} parse
//   config.cpp 231-233             -- reranker.model_dir parse
//   config.cpp 237-239             -- query_complexity.model_dir parse
//   config.cpp 243-258             -- gc.* parse block
//   config.cpp 450-468             -- gc.* ValidateConfig arms
//   file_based_global_config.cpp 31  -- JsonValueToString null -> ""
//   file_based_global_config.cpp 34  -- JsonValueToString object/array -> dump()
//   file_based_global_config.cpp 157-161 -- SetAgentLlmConfig round-trip
//
// Deterministic: temp files only (unique per pid + atomic counter, so -j parallel
// runs never collide), no network/LLM. Suite names are module-prefixed + globally
// unique (ConfigArms / FileGlobalConfigArms).

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "cortrix/common/agent_llm_config.h"
#include "cortrix/common/status.h"
#include "cortrix/config/config.h"
#include "cortrix/config/file_based_global_config.h"

namespace cortrix {
namespace {

// Unique temp path per process + atomic counter (defends against -j2 races where
// two test binaries / threads would otherwise reuse the same file name).
std::string UniqueTempPath(const std::string& suffix) {
    static std::atomic<unsigned> ctr{0};
    return std::string("/tmp/cortrix_config_arms_") + std::to_string(::getpid()) + "_" +
           std::to_string(ctr.fetch_add(1)) + "_" + suffix;
}

// Write `content` to a fresh unique temp file and return its path.
std::string WriteTemp(const std::string& content, const std::string& suffix) {
    std::string path = UniqueTempPath(suffix);
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

bool HasErr(const std::vector<std::string>& errs, const std::string& needle) {
    for (const auto& e : errs) {
        if (e.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ===========================================================================
// config.cpp -- YAML-parse sections not covered by test_config.cpp.
// ===========================================================================

// namespace.load_timeout_ms_per_ns parse (config.cpp:89) + the remaining ns fields.
TEST(ConfigArms, LoadNamespaceLoadTimeoutFromYaml) {
    std::string path = WriteTemp(R"(
namespace:
  data_dir: "/data/ns"
  max_active: 7
  idle_timeout_s: 120
  load_timeout_ms_per_ns: 42000
)",
                                 "ns.yaml");
    auto config = LoadConfig(path);
    EXPECT_EQ(config.ns.data_dir, "/data/ns");
    EXPECT_EQ(config.ns.max_active, 7);
    EXPECT_EQ(config.ns.idle_timeout_s, 120);
    EXPECT_EQ(config.ns.load_timeout_ms_per_ns, 42000);
    std::remove(path.c_str());
}

// memory.decay.{lambda,min_score} parse (config.cpp:216-219).
TEST(ConfigArms, LoadMemoryDecayFromYaml) {
    std::string path = WriteTemp(R"(
memory:
  default_ttl_seconds: 100
  decay:
    lambda: 0.05
    min_score: 0.25
)",
                                 "decay.yaml");
    auto config = LoadConfig(path);
    EXPECT_DOUBLE_EQ(config.memory.decay_lambda, 0.05);
    EXPECT_DOUBLE_EQ(config.memory.decay_min_score, 0.25);
    std::remove(path.c_str());
}

// security.{admin_bind,allow_public_admin} parse (config.cpp:224-227).
TEST(ConfigArms, LoadSecurityFromYaml) {
    std::string path = WriteTemp(R"(
security:
  admin_bind: "0.0.0.0"
  allow_public_admin: true
)",
                                 "security.yaml");
    auto config = LoadConfig(path);
    EXPECT_EQ(config.security.admin_bind, "0.0.0.0");
    EXPECT_TRUE(config.security.allow_public_admin);
    std::remove(path.c_str());
}

// reranker.model_dir parse (config.cpp:231-233).
TEST(ConfigArms, LoadRerankerFromYaml) {
    std::string path = WriteTemp(R"(
reranker:
  model_dir: "/models/reranker"
)",
                                 "reranker.yaml");
    auto config = LoadConfig(path);
    EXPECT_EQ(config.reranker.model_dir, "/models/reranker");
    std::remove(path.c_str());
}

// query_complexity.model_dir parse (config.cpp:237-239).
TEST(ConfigArms, LoadQueryComplexityFromYaml) {
    std::string path = WriteTemp(R"(
query_complexity:
  model_dir: "/models/qc"
)",
                                 "qc.yaml");
    auto config = LoadConfig(path);
    EXPECT_EQ(config.query_complexity.model_dir, "/models/qc");
    std::remove(path.c_str());
}

// gc.* full parse block (config.cpp:243-258).
TEST(ConfigArms, LoadGcFromYaml) {
    std::string path = WriteTemp(R"(
gc:
  enabled: false
  soft_delete_retention_days: 14
  blob_gc_retention_days: 45
  scan_interval_hours: 12
  max_purge_per_run: 5000
  max_run_duration_minutes: 10
  dry_run: true
  immediate_purge_enabled: true
)",
                                 "gc.yaml");
    auto config = LoadConfig(path);
    EXPECT_FALSE(config.gc.enabled);
    EXPECT_EQ(config.gc.soft_delete_retention_days, 14);
    EXPECT_EQ(config.gc.blob_gc_retention_days, 45);
    EXPECT_EQ(config.gc.scan_interval_hours, 12);
    EXPECT_EQ(config.gc.max_purge_per_run, 5000);
    EXPECT_EQ(config.gc.max_run_duration_minutes, 10);
    EXPECT_TRUE(config.gc.dry_run);
    EXPECT_TRUE(config.gc.immediate_purge_enabled);
    std::remove(path.c_str());
}

// ===========================================================================
// config.cpp -- gc.* ValidateConfig arms (config.cpp:450-468).
// ===========================================================================

TEST(ConfigArms, ValidateGcSoftDeleteRetentionNegative) {
    CortrixConfig config;
    config.gc.soft_delete_retention_days = -1;
    EXPECT_TRUE(HasErr(ValidateConfig(config), "gc.soft_delete_retention_days"));
}

TEST(ConfigArms, ValidateGcBlobRetentionNegative) {
    CortrixConfig config;
    config.gc.blob_gc_retention_days = -1;
    EXPECT_TRUE(HasErr(ValidateConfig(config), "gc.blob_gc_retention_days"));
}

TEST(ConfigArms, ValidateGcScanIntervalZero) {
    CortrixConfig config;
    config.gc.scan_interval_hours = 0;  // must be >= 1
    EXPECT_TRUE(HasErr(ValidateConfig(config), "gc.scan_interval_hours"));
}

TEST(ConfigArms, ValidateGcMaxPurgeZero) {
    CortrixConfig config;
    config.gc.max_purge_per_run = 0;  // must be >= 1
    EXPECT_TRUE(HasErr(ValidateConfig(config), "gc.max_purge_per_run"));
}

TEST(ConfigArms, ValidateGcMaxRunDurationZero) {
    CortrixConfig config;
    config.gc.max_run_duration_minutes = 0;  // must be >= 1
    EXPECT_TRUE(HasErr(ValidateConfig(config), "gc.max_run_duration_minutes"));
}

TEST(ConfigArms, ValidateGcDefaultsPass) {
    CortrixConfig config;  // GcConfig defaults are all valid
    EXPECT_FALSE(HasErr(ValidateConfig(config), "gc."));
}

// ===========================================================================
// file_based_global_config.cpp -- JsonValueToString null/object/array arms
// (lines 31, 34) + SetAgentLlmConfig round-trip (lines 157-161).
// ===========================================================================

// A JSON null value coerces to "" (file_based_global_config.cpp:31). The key is
// still present (NotFound only fires for absent keys), and reads back as "".
TEST(FileGlobalConfigArms, NullValueCoercesToEmptyString) {
    std::string path = WriteTemp(R"({"present": "x", "blank": null})", "null.json");
    config::FileBasedGlobalConfig cfg(path);
    Result<std::string> blank = cfg.GetString("blank");
    ASSERT_TRUE(blank.ok()) << blank.status().message();
    EXPECT_EQ(blank.value(), "");
    EXPECT_EQ(cfg.GetString("present").value(), "x");
    std::remove(path.c_str());
}

// An object value is stored as its compact JSON dump (file_based_global_config.cpp:34).
TEST(FileGlobalConfigArms, ObjectValueStoredAsCompactJson) {
    std::string path = WriteTemp(R"({"obj": {"a": 1, "b": "two"}, "arr": [1, 2, 3]})", "obj.json");
    config::FileBasedGlobalConfig cfg(path);
    Result<std::string> obj = cfg.GetString("obj");
    ASSERT_TRUE(obj.ok()) << obj.status().message();
    // nlohmann compact dump has no spaces: {"a":1,"b":"two"}.
    EXPECT_NE(obj.value().find("\"a\":1"), std::string::npos) << obj.value();
    EXPECT_NE(obj.value().find("\"b\":\"two\""), std::string::npos) << obj.value();
    Result<std::string> arr = cfg.GetString("arr");
    ASSERT_TRUE(arr.ok());
    EXPECT_EQ(arr.value(), "[1,2,3]");
    std::remove(path.c_str());
}

// Numeric / boolean scalars coerce to their textual form (covers the
// is_boolean / is_number_integer / is_number_float arms of JsonValueToString).
TEST(FileGlobalConfigArms, ScalarValuesCoerceToText) {
    std::string path =
        WriteTemp(R"({"b": true, "i": 42, "f": 1.5, "s": "hi"})", "scalar.json");
    config::FileBasedGlobalConfig cfg(path);
    EXPECT_EQ(cfg.GetString("b").value(), "true");
    EXPECT_EQ(cfg.GetString("i").value(), "42");
    EXPECT_EQ(cfg.GetString("s").value(), "hi");
    // float coercion uses ostringstream default formatting -> "1.5".
    EXPECT_EQ(cfg.GetString("f").value(), "1.5");
    // The coerced bool/int round-trip through the typed getters too.
    EXPECT_TRUE(cfg.GetBool("b").value());
    EXPECT_EQ(cfg.GetInt("i").value(), 42);
    std::remove(path.c_str());
}

// SetAgentLlmConfig stores the 6 agent_llm.* keys via the shared codec; the
// inherited GetAgentLlmConfig reads them back (api_key decrypted). Covers
// file_based_global_config.cpp:157-161.
TEST(FileGlobalConfigArms, SetAgentLlmConfigRoundTrip) {
    config::FileBasedGlobalConfig cfg;  // empty, in-memory
    AgentLlmConfig in;
    in.provider = "openai";
    in.api_key = "sk-arms-secret";
    in.model = "gpt-4o-mini";
    in.base_url = "https://llm.example.com/v1";
    in.max_tokens = 2048;
    in.temperature = 0.3;

    cfg.SetAgentLlmConfig(in);

    // The codec persists agent_llm.provider as a plaintext key.
    Result<std::string> prov = cfg.GetString("agent_llm.provider");
    ASSERT_TRUE(prov.ok()) << prov.status().message();
    EXPECT_EQ(prov.value(), "openai");

    // Full round-trip through the canonical getter (api_key decrypted in memory).
    AgentLlmConfig out = cfg.GetAgentLlmConfig();
    EXPECT_EQ(out.provider, "openai");
    EXPECT_EQ(out.model, "gpt-4o-mini");
    EXPECT_EQ(out.base_url, "https://llm.example.com/v1");
    EXPECT_EQ(out.max_tokens, 2048);
    EXPECT_FLOAT_EQ(out.temperature, 0.3f);  // temperature is float, not double
    EXPECT_EQ(out.api_key, "sk-arms-secret");
}

}  // namespace
}  // namespace cortrix
