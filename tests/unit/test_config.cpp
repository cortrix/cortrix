#include <gtest/gtest.h>
#include <fstream>
#include <cstdlib>
#include "cortrix/config/config.h"

namespace cortrix {
namespace {

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_config_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());
        // Clear all env vars we might set
        unsetenv("CORTRIX_SERVER_HOST");
        unsetenv("CORTRIX_SERVER_PORT");
        unsetenv("CORTRIX_SERVER_THREADS");
        unsetenv("CORTRIX_LOG_LEVEL");
        unsetenv("CORTRIX_LOG_FORMAT");
        unsetenv("CORTRIX_LOG_OUTPUT");
        unsetenv("CORTRIX_DATA_DIR");
        unsetenv("CORTRIX_AUTH_ENABLED");
    }

    void TearDown() override {
        system(("rm -rf " + tmp_dir_).c_str());
        // Clean up env vars
        unsetenv("CORTRIX_SERVER_HOST");
        unsetenv("CORTRIX_SERVER_PORT");
        unsetenv("CORTRIX_SERVER_THREADS");
        unsetenv("CORTRIX_LOG_LEVEL");
        unsetenv("CORTRIX_LOG_FORMAT");
        unsetenv("CORTRIX_LOG_OUTPUT");
        unsetenv("CORTRIX_DATA_DIR");
        unsetenv("CORTRIX_AUTH_ENABLED");
    }

    void WriteYaml(const std::string& content) {
        yaml_path_ = tmp_dir_ + "/cortrix.yaml";
        std::ofstream f(yaml_path_);
        f << content;
        f.close();
    }

    std::string tmp_dir_;
    std::string yaml_path_;
};

TEST_F(ConfigTest, DefaultValues) {
    auto config = LoadConfig("");
    EXPECT_EQ(config.server.host, "0.0.0.0");
    EXPECT_EQ(config.server.port, 8420);
    EXPECT_EQ(config.server.thread_count, 8);
    EXPECT_TRUE(config.auth.enabled);
    EXPECT_EQ(config.log.level, "info");
    EXPECT_EQ(config.log.format, "json");
    EXPECT_EQ(config.log.output, "stdout");
    EXPECT_EQ(config.ns.data_dir, "./data");
}

TEST_F(ConfigTest, LoadFromYaml) {
    WriteYaml(R"(
server:
  host: "127.0.0.1"
  port: 9090
  thread_count: 4
  max_payload_bytes: 52428800

auth:
  enabled: false
  api_keys:
    - key_hash: "abc123"
      tenant_id: "tenant_001"
      permissions: 7
      expires_at: 0

log:
  level: "debug"
  format: "text"
  output: "stderr"

namespace:
  data_dir: "/tmp/test_data"
  max_active: 10
  idle_timeout_s: 600
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.server.host, "127.0.0.1");
    EXPECT_EQ(config.server.port, 9090);
    EXPECT_EQ(config.server.thread_count, 4);
    EXPECT_EQ(config.server.max_payload_bytes, 52428800);
    EXPECT_FALSE(config.auth.enabled);
    ASSERT_EQ(config.auth.api_keys.size(), 1u);
    EXPECT_EQ(config.auth.api_keys[0].key_hash, "abc123");
    EXPECT_EQ(config.auth.api_keys[0].tenant_id, "tenant_001");
    EXPECT_EQ(config.auth.api_keys[0].permissions, 7);
    EXPECT_EQ(config.log.level, "debug");
    EXPECT_EQ(config.log.format, "text");
    EXPECT_EQ(config.log.output, "stderr");
    EXPECT_EQ(config.ns.data_dir, "/tmp/test_data");
    EXPECT_EQ(config.ns.max_active, 10);
    EXPECT_EQ(config.ns.idle_timeout_s, 600);
}

// [D3.5 gap①] The two ingest-side LLM roles parse from yaml: enricher_llm (F03
// plan B, §2.7.bis) and doc_summary_llm (F41 — its parse was MISSING when F42-1
// landed the field, so a configured block silently never reached IsConfigured()).
TEST_F(ConfigTest, IngestLlmRolesParsed) {
    WriteYaml(R"(
doc_summary_llm:
  provider: "openai"
  api_key: "sk-docsum"
  model: "gpt-4o-mini"

enricher_llm:
  provider: "openai"
  api_key: "sk-enrich"
  model: "gpt-4o-mini"
  base_url: "https://llm.example.com/v1"
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_TRUE(config.doc_summary_llm.IsConfigured());
    EXPECT_EQ(config.doc_summary_llm.api_key, "sk-docsum");
    EXPECT_TRUE(config.enricher_llm.IsConfigured());
    EXPECT_EQ(config.enricher_llm.api_key, "sk-enrich");
    EXPECT_EQ(config.enricher_llm.base_url, "https://llm.example.com/v1");
    EXPECT_EQ(config.enricher_llm.model, "gpt-4o-mini");
}

TEST_F(ConfigTest, IngestLlmRolesDefaultUnconfigured) {
    auto config = LoadConfig("");
    EXPECT_FALSE(config.doc_summary_llm.IsConfigured());
    EXPECT_FALSE(config.enricher_llm.IsConfigured());
}

// Deep-QA 2026-07-10 config seams: F35 ctx knobs on enricher_llm, parser
// hard-fail gate, F42 sweeper pacing. Absent = 0/false (built-ins unchanged).
TEST_F(ConfigTest, DeepQaConfigSeamsParsed) {
    WriteYaml(R"(
enricher_llm:
  provider: "glm"
  api_key: "sk-enrich"
  model: "glm-4-flash"
  max_tokens: 8192
  ctx_max_output_tokens: 120
  ctx_guard_chars_per_token: 6

spc:
  require_parsers: true
  enrich_sweep_interval_sec: 15
  enrich_sweep_batch: 128
)");
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.enricher_llm.max_tokens, 8192);
    EXPECT_EQ(config.enricher_llm.ctx_max_output_tokens, 120);
    EXPECT_EQ(config.enricher_llm.ctx_guard_chars_per_token, 6);
    EXPECT_TRUE(config.spc.require_parsers);
    EXPECT_EQ(config.spc.enrich_sweep_interval_sec, 15);
    EXPECT_EQ(config.spc.enrich_sweep_batch, 128);

    auto absent = LoadConfig("");
    EXPECT_EQ(absent.enricher_llm.max_tokens, 0);
    EXPECT_EQ(absent.enricher_llm.ctx_max_output_tokens, 0);
    EXPECT_EQ(absent.enricher_llm.ctx_guard_chars_per_token, 0);
    EXPECT_FALSE(absent.spc.require_parsers);
    EXPECT_EQ(absent.spc.enrich_sweep_interval_sec, 0);
    EXPECT_EQ(absent.spc.enrich_sweep_batch, 0);
}

// F38 §4.3 as-built (2026-07-10): enricher_llm.hype_questions_per_chunk reaches
// the parsed config; absent = 0 (consumer keeps the built-in default 3).
TEST_F(ConfigTest, EnricherHypeQuestionsPerChunkParsed) {
    WriteYaml(R"(
enricher_llm:
  provider: "glm"
  api_key: "sk-enrich"
  model: "glm-4-flash"
  hype_questions_per_chunk: 6
)");
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.enricher_llm.hype_questions_per_chunk, 6);

    WriteYaml(R"(
enricher_llm:
  provider: "glm"
  api_key: "sk-enrich"
  model: "glm-4-flash"
)");
    auto absent = LoadConfig(yaml_path_);
    EXPECT_EQ(absent.enricher_llm.hype_questions_per_chunk, 0);
}

TEST_F(ConfigTest, EnvOverridesYaml) {
    WriteYaml(R"(
server:
  port: 8080
log:
  level: "info"
)");

    setenv("CORTRIX_SERVER_PORT", "9090", 1);
    setenv("CORTRIX_LOG_LEVEL", "debug", 1);

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.server.port, 9090);
    EXPECT_EQ(config.log.level, "debug");
}

// Retrieval candidate-pool sizing: default preserves the historical 3 / 50,
// yaml configures it, env wins over yaml. Raising max_candidates lifts the
// pool ceiling that otherwise caps recall on large corpora.
TEST_F(ConfigTest, RetrievalPoolDefault) {
    auto config = LoadConfig("");
    EXPECT_EQ(config.retrieval.candidate_multiplier, 3);
    EXPECT_EQ(config.retrieval.max_candidates, 50);
}

TEST_F(ConfigTest, RetrievalPoolFromYaml) {
    WriteYaml(R"(
retrieval:
  candidate_multiplier: 5
  max_candidates: 400
)");
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.retrieval.candidate_multiplier, 5);
    EXPECT_EQ(config.retrieval.max_candidates, 400);
}

TEST_F(ConfigTest, RetrievalPoolEnvOverridesYaml) {
    WriteYaml(R"(
retrieval:
  max_candidates: 400
)");
    setenv("CORTRIX_RETRIEVAL_MAX_CANDIDATES", "1000", 1);
    setenv("CORTRIX_RETRIEVAL_CANDIDATE_MULTIPLIER", "8", 1);
    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.retrieval.max_candidates, 1000);
    EXPECT_EQ(config.retrieval.candidate_multiplier, 8);
    unsetenv("CORTRIX_RETRIEVAL_MAX_CANDIDATES");
    unsetenv("CORTRIX_RETRIEVAL_CANDIDATE_MULTIPLIER");
}

TEST_F(ConfigTest, MissingYamlFile) {
    auto config = LoadConfig("/nonexistent/path/cortrix.yaml");
    // Should not crash, use defaults
    EXPECT_EQ(config.server.port, 8420);
    EXPECT_EQ(config.log.level, "info");
}

TEST_F(ConfigTest, InvalidYaml) {
    WriteYaml("{{{{invalid yaml content!!!!");

    auto config = LoadConfig(yaml_path_);
    // Should not crash, use defaults
    EXPECT_EQ(config.server.port, 8420);
}

TEST_F(ConfigTest, PartialYaml) {
    WriteYaml(R"(
server:
  port: 3000
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.server.port, 3000);
    EXPECT_EQ(config.server.host, "0.0.0.0");  // default
    EXPECT_EQ(config.log.level, "info");  // default
    EXPECT_EQ(config.ns.data_dir, "./data");  // default
}

TEST_F(ConfigTest, EnvBoolParsing) {
    setenv("CORTRIX_AUTH_ENABLED", "false", 1);

    auto config = LoadConfig("");
    EXPECT_FALSE(config.auth.enabled);
}

TEST_F(ConfigTest, EnvBoolParsingNumeric) {
    setenv("CORTRIX_AUTH_ENABLED", "0", 1);

    auto config = LoadConfig("");
    EXPECT_FALSE(config.auth.enabled);
}

TEST_F(ConfigTest, EnvStringOverrides) {
    setenv("CORTRIX_SERVER_HOST", "localhost", 1);
    setenv("CORTRIX_DATA_DIR", "/custom/data", 1);
    setenv("CORTRIX_LOG_FORMAT", "text", 1);
    setenv("CORTRIX_LOG_OUTPUT", "/var/log/cortrix.log", 1);

    auto config = LoadConfig("");
    EXPECT_EQ(config.server.host, "localhost");
    EXPECT_EQ(config.ns.data_dir, "/custom/data");
    EXPECT_EQ(config.log.format, "text");
    EXPECT_EQ(config.log.output, "/var/log/cortrix.log");
}

// --- ValidateConfig tests ---

TEST_F(ConfigTest, ValidateDefaultConfigPasses) {
    auto config = LoadConfig("");
    auto errors = ValidateConfig(config);
    EXPECT_TRUE(errors.empty()) << "Default config should pass validation";
}

TEST_F(ConfigTest, ValidatePortZero) {
    CortrixConfig config;
    config.server.port = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("server.port") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "port=0 should fail validation";
}

TEST_F(ConfigTest, ValidatePortNegative) {
    CortrixConfig config;
    config.server.port = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("server.port") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "port=-1 should fail validation";
}

TEST_F(ConfigTest, ValidatePortTooHigh) {
    CortrixConfig config;
    config.server.port = 65536;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("server.port") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "port=65536 should fail validation";
}

TEST_F(ConfigTest, ValidatePortBoundaryValid) {
    CortrixConfig config;
    config.server.port = 1;
    auto errors = ValidateConfig(config);
    bool found_port = false;
    for (const auto& e : errors) {
        if (e.find("server.port") != std::string::npos) { found_port = true; break; }
    }
    EXPECT_FALSE(found_port) << "port=1 should pass";

    config.server.port = 65535;
    errors = ValidateConfig(config);
    found_port = false;
    for (const auto& e : errors) {
        if (e.find("server.port") != std::string::npos) { found_port = true; break; }
    }
    EXPECT_FALSE(found_port) << "port=65535 should pass";
}

TEST_F(ConfigTest, ValidateThreadCountZero) {
    CortrixConfig config;
    config.server.thread_count = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("thread_count") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "thread_count=0 should fail";
}

TEST_F(ConfigTest, ValidateMaxPayloadZero) {
    CortrixConfig config;
    config.server.max_payload_bytes = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_payload_bytes") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_payload_bytes=0 should fail";
}

TEST_F(ConfigTest, ValidateEmptyDataDir) {
    CortrixConfig config;
    config.ns.data_dir = "";
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("data_dir") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "empty data_dir should fail";
}

TEST_F(ConfigTest, ValidateMaxActiveZero) {
    CortrixConfig config;
    config.ns.max_active = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_active") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_active=0 should fail";
}

TEST_F(ConfigTest, ValidateIdleTimeoutNegative) {
    CortrixConfig config;
    config.ns.idle_timeout_s = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("idle_timeout_s") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "idle_timeout_s=-1 should fail";
}

TEST_F(ConfigTest, ValidateEmbeddingDimensionZero) {
    CortrixConfig config;
    config.embedding.dimension = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("embedding.dimension") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "dimension=0 should fail";
}

TEST_F(ConfigTest, ValidateWorkerCountZero) {
    // Design (config.cpp): spc.worker_count = 0 means auto-detect (valid); only negative is rejected.
    auto has_worker_err = [](const std::vector<std::string>& errs) {
        for (const auto& e : errs) {
            if (e.find("worker_count") != std::string::npos) return true;
        }
        return false;
    };
    CortrixConfig config;
    config.spc.worker_count = 0;
    EXPECT_FALSE(has_worker_err(ValidateConfig(config)))
        << "worker_count=0 should be valid (auto-detect)";

    config.spc.worker_count = -1;
    EXPECT_TRUE(has_worker_err(ValidateConfig(config)))
        << "worker_count<0 should fail";
}

TEST_F(ConfigTest, ValidateChunkSizeZero) {
    CortrixConfig config;
    config.spc.chunk_size = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("chunk_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "chunk_size=0 should fail";
}

TEST_F(ConfigTest, ValidateChunkOverlapNegative) {
    CortrixConfig config;
    config.spc.chunk_overlap = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("chunk_overlap") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "chunk_overlap=-1 should fail";
}

TEST_F(ConfigTest, ValidateChunkOverlapExceedsChunkSize) {
    CortrixConfig config;
    config.spc.chunk_size = 100;
    config.spc.chunk_overlap = 100;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("chunk_overlap must be < spc.chunk_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "chunk_overlap=chunk_size should fail";
}

TEST_F(ConfigTest, ValidateInvalidLogLevel) {
    CortrixConfig config;
    config.log.level = "verbose";
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("log.level") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "invalid log level should fail";
}

TEST_F(ConfigTest, ValidateInvalidLogFormat) {
    CortrixConfig config;
    config.log.format = "xml";
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("log.format") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "invalid log format should fail";
}

TEST_F(ConfigTest, ValidateUploadMaxFileSizeZero) {
    CortrixConfig config;
    config.upload.max_file_size = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("upload.max_file_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_file_size=0 should fail";
}

TEST_F(ConfigTest, ValidateScanBatchSizeNegative) {
    CortrixConfig config;
    config.watch_dir.scan_batch_size = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("scan_batch_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "scan_batch_size=-1 should fail";
}

TEST_F(ConfigTest, ValidateMultipleErrors) {
    CortrixConfig config;
    config.server.port = 0;
    config.server.thread_count = 0;
    config.ns.data_dir = "";
    config.log.level = "invalid";
    auto errors = ValidateConfig(config);
    EXPECT_GE(errors.size(), 4u) << "Should detect multiple validation errors";
}

TEST_F(ConfigTest, ValidateAllLogLevels) {
    std::vector<std::string> valid_levels = {"trace", "debug", "info", "warn", "error", "critical", "off"};
    for (const auto& level : valid_levels) {
        CortrixConfig config;
        config.log.level = level;
        auto errors = ValidateConfig(config);
        bool found = false;
        for (const auto& e : errors) {
            if (e.find("log.level") != std::string::npos) { found = true; break; }
        }
        EXPECT_FALSE(found) << "log level '" << level << "' should be valid";
    }
}

// --- YAML loading for SPC, upload, watch_dir, memory sections ---

TEST_F(ConfigTest, LoadSpcFromYaml) {
    WriteYaml(R"(
spc:
  worker_count: 4
  max_queue_size: 5000
  task_timeout_s: 600
  python_bin: "/usr/bin/python3"
  chunk_size: 256
  chunk_overlap: 32
  embedding_batch_size: 8
  onnx_intra_threads: 2
  onnx_inter_threads: 2
  ocr_timeout_s: 120
  ocr_confidence_threshold: 0.5
  ocr_enable_cls: false
  enable_crash_recovery: false
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.spc.worker_count, 4);
    EXPECT_EQ(config.spc.max_queue_size, 5000);
    EXPECT_EQ(config.spc.task_timeout_s, 600);
    EXPECT_EQ(config.spc.python_bin, "/usr/bin/python3");
    EXPECT_EQ(config.spc.chunk_size, 256);
    EXPECT_EQ(config.spc.chunk_overlap, 32);
    EXPECT_EQ(config.spc.embedding_batch_size, 8);
    EXPECT_EQ(config.spc.onnx_intra_threads, 2);
    EXPECT_EQ(config.spc.onnx_inter_threads, 2);
    EXPECT_EQ(config.spc.ocr_timeout_s, 120);
    EXPECT_FLOAT_EQ(config.spc.ocr_confidence_threshold, 0.5f);
    EXPECT_FALSE(config.spc.ocr_enable_cls);
    EXPECT_FALSE(config.spc.enable_crash_recovery);
}

TEST_F(ConfigTest, LoadUploadFromYaml) {
    WriteYaml(R"(
upload:
  max_file_size: 52428800
  large_file_threshold: 10485760
  allowed_mime_types: ["application/pdf", "text/plain"]
  temp_dir: "/custom/tmp"
  max_concurrent_uploads: 5
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.upload.max_file_size, 52428800);
    EXPECT_EQ(config.upload.large_file_threshold, 10485760);
    ASSERT_EQ(config.upload.allowed_mime_types.size(), 2u);
    EXPECT_EQ(config.upload.allowed_mime_types[0], "application/pdf");
    EXPECT_EQ(config.upload.allowed_mime_types[1], "text/plain");
    EXPECT_EQ(config.upload.temp_dir, "/custom/tmp");
    EXPECT_EQ(config.upload.max_concurrent_uploads, 5);
}

TEST_F(ConfigTest, LoadWatchDirFromYaml) {
    WriteYaml(R"(
watch_dir:
  data_dir: "/data/watch"
  namespace_name: "production"
  watch_enabled: false
  fsevents_latency_s: 1.0
  scan_batch_size: 200
  max_concurrent_imports: 10
  ignore_patterns: ["*.log", "*.bak"]
  allowed_extensions: [".pdf", ".md"]
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.watch_dir.data_dir, "/data/watch");
    EXPECT_EQ(config.watch_dir.namespace_name, "production");
    EXPECT_FALSE(config.watch_dir.watch_enabled);
    EXPECT_DOUBLE_EQ(config.watch_dir.fsevents_latency_s, 1.0);
    EXPECT_EQ(config.watch_dir.scan_batch_size, 200);
    EXPECT_EQ(config.watch_dir.max_concurrent_imports, 10);
    ASSERT_EQ(config.watch_dir.ignore_patterns.size(), 2u);
    EXPECT_EQ(config.watch_dir.ignore_patterns[0], "*.log");
    EXPECT_EQ(config.watch_dir.ignore_patterns[1], "*.bak");
    ASSERT_EQ(config.watch_dir.allowed_extensions.size(), 2u);
    EXPECT_EQ(config.watch_dir.allowed_extensions[0], ".pdf");
    EXPECT_EQ(config.watch_dir.allowed_extensions[1], ".md");
}

TEST_F(ConfigTest, LoadMemoryFromYaml) {
    WriteYaml(R"(
memory:
  default_ttl_seconds: 3600
  text_to_sql_ttl_seconds: 7200
  inject_recent_turns: 10
  inject_max_tokens: 4000
  max_sessions_per_namespace: 5000
  max_interactions_per_session: 500
  chunk_strategy: "per_message"
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.memory.default_ttl_seconds, 3600);
    EXPECT_EQ(config.memory.text_to_sql_ttl_seconds, 7200);
    EXPECT_EQ(config.memory.inject_recent_turns, 10);
    EXPECT_EQ(config.memory.inject_max_tokens, 4000);
    EXPECT_EQ(config.memory.max_sessions_per_namespace, 5000);
    EXPECT_EQ(config.memory.max_interactions_per_session, 500);
    EXPECT_EQ(config.memory.chunk_strategy, "per_message");
}

// --- New validation tests ---

TEST_F(ConfigTest, ValidateWatchDirMaxConcurrentImportsNegative) {
    CortrixConfig config;
    config.watch_dir.max_concurrent_imports = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_concurrent_imports") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_concurrent_imports=-1 should fail";
}

TEST_F(ConfigTest, ValidateWatchDirMaxConcurrentImportsZeroValid) {
    CortrixConfig config;
    config.watch_dir.max_concurrent_imports = 0;  // 0 means unlimited
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_concurrent_imports") != std::string::npos) { found = true; break; }
    }
    EXPECT_FALSE(found) << "max_concurrent_imports=0 should pass (unlimited)";
}

TEST_F(ConfigTest, ValidateTaskTimeoutNegative) {
    CortrixConfig config;
    config.spc.task_timeout_s = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("task_timeout_s") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "task_timeout_s=-1 should fail";
}

TEST_F(ConfigTest, ValidateParserTimeoutNegative) {
    CortrixConfig config;
    config.spc.parser_timeout_s = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("parser_timeout_s") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "parser_timeout_s=-1 should fail";
}

TEST_F(ConfigTest, ValidateEmbeddingBatchSizeZero) {
    CortrixConfig config;
    config.spc.embedding_batch_size = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("embedding_batch_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "embedding_batch_size=0 should fail";
}

TEST_F(ConfigTest, ValidateMemoryDefaultTtlNegative) {
    CortrixConfig config;
    config.memory.default_ttl_seconds = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("default_ttl_seconds") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "default_ttl_seconds=-1 should fail";
}

TEST_F(ConfigTest, ValidateMemoryInjectRecentTurnsNegative) {
    CortrixConfig config;
    config.memory.inject_recent_turns = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("inject_recent_turns") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "inject_recent_turns=-1 should fail";
}

TEST_F(ConfigTest, ValidateMemoryInjectMaxTokensNegative) {
    CortrixConfig config;
    config.memory.inject_max_tokens = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("inject_max_tokens") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "inject_max_tokens=-1 should fail";
}

TEST_F(ConfigTest, ValidateMemoryMaxSessionsZero) {
    CortrixConfig config;
    config.memory.max_sessions_per_namespace = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_sessions_per_namespace") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_sessions_per_namespace=0 should fail";
}

TEST_F(ConfigTest, ValidateMemoryMaxInteractionsZero) {
    CortrixConfig config;
    config.memory.max_interactions_per_session = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_interactions_per_session") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_interactions_per_session=0 should fail";
}

TEST_F(ConfigTest, ValidateMemoryDefaultTtlZeroValid) {
    CortrixConfig config;
    config.memory.default_ttl_seconds = 0;  // 0 means no TTL
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("default_ttl_seconds") != std::string::npos) { found = true; break; }
    }
    EXPECT_FALSE(found) << "default_ttl_seconds=0 should pass (no TTL)";
}

TEST_F(ConfigTest, ValidateFullConfigFromYaml) {
    WriteYaml(R"(
server:
  host: "0.0.0.0"
  port: 8080
  thread_count: 8
  max_payload_bytes: 104857600
log:
  level: "info"
  format: "json"
namespace:
  data_dir: "/data"
  max_active: 20
  idle_timeout_s: 300
embedding:
  dimension: 1024
spc:
  worker_count: 2
  chunk_size: 512
  chunk_overlap: 50
  task_timeout_s: 300
  parser_timeout_s: 120
  embedding_batch_size: 4
upload:
  max_file_size: 104857600
watch_dir:
  scan_batch_size: 100
  max_concurrent_imports: 50
memory:
  default_ttl_seconds: 0
  inject_recent_turns: 5
  inject_max_tokens: 2000
  max_sessions_per_namespace: 10000
  max_interactions_per_session: 1000
)");

    auto config = LoadConfig(yaml_path_);
    auto errors = ValidateConfig(config);
    EXPECT_TRUE(errors.empty()) << "Full valid YAML config should pass validation. Errors: "
        << (errors.empty() ? "" : errors[0]);
}

// ============================================================
// Coverage Boost: GetEnvBool edge cases, YAML section loading
// ============================================================

TEST_F(ConfigTest, EnvBoolFalseForRandomString) {
    setenv("CORTRIX_AUTH_ENABLED", "yes", 1);  // not "true" or "1"
    auto config = LoadConfig("");
    // "yes" != "true" and != "1" -> returns false
    EXPECT_FALSE(config.auth.enabled);
}

TEST_F(ConfigTest, EnvIntInvalidFallsBackToDefault) {
    setenv("CORTRIX_SERVER_PORT", "not_a_number", 1);
    auto config = LoadConfig("");
    EXPECT_EQ(config.server.port, 8420);  // default
}

TEST_F(ConfigTest, EnvIntZero) {
    setenv("CORTRIX_SERVER_PORT", "0", 1);
    auto config = LoadConfig("");
    EXPECT_EQ(config.server.port, 0);
}

TEST_F(ConfigTest, EnvServerThreads) {
    setenv("CORTRIX_SERVER_THREADS", "16", 1);
    auto config = LoadConfig("");
    EXPECT_EQ(config.server.thread_count, 16);
}

TEST_F(ConfigTest, LoadEmbeddingFromYaml) {
    WriteYaml(R"(
embedding:
  model_path: "/models/bge-m3"
  dimension: 768
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.embedding.model_path, "/models/bge-m3");
    EXPECT_EQ(config.embedding.dimension, 768);
}

TEST_F(ConfigTest, LoadLlmFromYaml) {
    WriteYaml(R"(
llm:
  provider: "openai"
  api_key: "sk-test123"
  model: "gpt-4"
  base_url: "https://api.openai.com"
)");

    auto config = LoadConfig(yaml_path_);
    // "llm:" YAML key maps to semantic_llm (backward compat, see config.cpp)
    EXPECT_EQ(config.semantic_llm.provider, "openai");
    EXPECT_EQ(config.semantic_llm.api_key, "sk-test123");
    EXPECT_EQ(config.semantic_llm.model, "gpt-4");
    EXPECT_EQ(config.semantic_llm.base_url, "https://api.openai.com");
}

TEST_F(ConfigTest, LoadSpcScriptsFromYaml) {
    WriteYaml(R"(
spc:
  parse_pdf_script: "/scripts/parse_pdf.py"
  parse_word_script: "/scripts/parse_word.py"
  ocr_script: "/scripts/ocr.py"
  parser_timeout_s: 60
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.spc.parse_pdf_script, "/scripts/parse_pdf.py");
    EXPECT_EQ(config.spc.parse_word_script, "/scripts/parse_word.py");
    EXPECT_EQ(config.spc.ocr_script, "/scripts/ocr.py");
    EXPECT_EQ(config.spc.parser_timeout_s, 60);
}

TEST_F(ConfigTest, YamlFileExistsButEmpty) {
    WriteYaml("");
    auto config = LoadConfig(yaml_path_);
    // Should use defaults
    EXPECT_EQ(config.server.port, 8420);
}

TEST_F(ConfigTest, ValidateMaxPayloadNegative) {
    CortrixConfig config;
    config.server.max_payload_bytes = -100;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_payload_bytes") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_payload_bytes=-100 should fail";
}

TEST_F(ConfigTest, ValidateChunkOverlapLessThanChunkSizeValid) {
    CortrixConfig config;
    config.spc.chunk_size = 100;
    config.spc.chunk_overlap = 99;
    auto errors = ValidateConfig(config);
    bool found_overlap = false;
    for (const auto& e : errors) {
        if (e.find("chunk_overlap must be < spc.chunk_size") != std::string::npos) {
            found_overlap = true; break;
        }
    }
    EXPECT_FALSE(found_overlap) << "chunk_overlap=99 < chunk_size=100 should pass";
}

TEST_F(ConfigTest, ValidateMaxActiveNegative) {
    CortrixConfig config;
    config.ns.max_active = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_active") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_active=-1 should fail";
}

TEST_F(ConfigTest, ValidateEmbeddingDimensionNegative) {
    CortrixConfig config;
    config.embedding.dimension = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("embedding.dimension") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "dimension=-1 should fail";
}

TEST_F(ConfigTest, ValidateMaxQueueSizeZeroValid) {
    CortrixConfig config;
    config.spc.max_queue_size = 0;
    // max_queue_size doesn't have a validation rule currently
    auto errors = ValidateConfig(config);
    // Should pass (no validation for max_queue_size)
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("max_queue_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_FALSE(found);
}

TEST_F(ConfigTest, LoadConfigNonexistentPathReturnsDefaults) {
    auto config = LoadConfig("/this/path/does/not/exist.yaml");
    // Should not crash, returns defaults
    EXPECT_EQ(config.server.host, "0.0.0.0");
    EXPECT_EQ(config.server.port, 8420);
    EXPECT_EQ(config.ns.data_dir, "./data");
    EXPECT_EQ(config.log.level, "info");
}

TEST_F(ConfigTest, EnvOverridesAuth) {
    setenv("CORTRIX_AUTH_ENABLED", "true", 1);

    auto config = LoadConfig("");
    EXPECT_TRUE(config.auth.enabled);
}

TEST_F(ConfigTest, ValidateAllLogFormats) {
    for (const auto& fmt : {"json", "text"}) {
        CortrixConfig config;
        config.log.format = fmt;
        auto errors = ValidateConfig(config);
        bool found = false;
        for (const auto& e : errors) {
            if (e.find("log.format") != std::string::npos) { found = true; break; }
        }
        EXPECT_FALSE(found) << "log format '" << fmt << "' should be valid";
    }
}

TEST_F(ConfigTest, ValidateUploadMaxFileSizeNegative) {
    CortrixConfig config;
    config.upload.max_file_size = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("upload.max_file_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "max_file_size=-1 should fail";
}

TEST_F(ConfigTest, ValidateScanBatchSizeZeroValid) {
    CortrixConfig config;
    config.watch_dir.scan_batch_size = 0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("scan_batch_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_FALSE(found) << "scan_batch_size=0 should pass";
}

// --- error_threshold validation ---

TEST_F(ConfigTest, ValidateErrorThresholdNegative) {
    CortrixConfig config;
    config.watch_dir.error_threshold = -1;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("error_threshold") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "error_threshold=-1 should fail";
}

TEST_F(ConfigTest, ValidateErrorThresholdZeroValid) {
    CortrixConfig config;
    config.watch_dir.error_threshold = 0;  // 0 = no limit
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("error_threshold") != std::string::npos) { found = true; break; }
    }
    EXPECT_FALSE(found) << "error_threshold=0 should pass (no limit)";
}

TEST_F(ConfigTest, ValidateErrorThresholdPositiveValid) {
    CortrixConfig config;
    config.watch_dir.error_threshold = 10;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("error_threshold") != std::string::npos) { found = true; break; }
    }
    EXPECT_FALSE(found) << "error_threshold=10 should pass";
}

// --- fsevents_latency_s validation ---

TEST_F(ConfigTest, ValidateFseventsLatencyNegative) {
    CortrixConfig config;
    config.watch_dir.fsevents_latency_s = -1.0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("fsevents_latency_s") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "fsevents_latency_s=-1.0 should fail";
}

TEST_F(ConfigTest, ValidateFseventsLatencyZeroValid) {
    CortrixConfig config;
    config.watch_dir.fsevents_latency_s = 0.0;
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("fsevents_latency_s") != std::string::npos) { found = true; break; }
    }
    EXPECT_FALSE(found) << "fsevents_latency_s=0.0 should pass";
}

// --- log.output validation ---

TEST_F(ConfigTest, ValidateLogOutputEmpty) {
    CortrixConfig config;
    config.log.output = "";
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("log.output") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "empty log.output should fail";
}

TEST_F(ConfigTest, ValidateLogOutputStdoutValid) {
    CortrixConfig config;
    config.log.output = "stdout";
    auto errors = ValidateConfig(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("log.output") != std::string::npos) { found = true; break; }
    }
    EXPECT_FALSE(found) << "log.output='stdout' should pass";
}

// --- YAML loading for error_threshold ---

TEST_F(ConfigTest, LoadErrorThresholdFromYaml) {
    WriteYaml(R"(
watch_dir:
  data_dir: "/data/watch"
  error_threshold: 50
)");

    auto config = LoadConfig(yaml_path_);
    EXPECT_EQ(config.watch_dir.error_threshold, 50);
}

}  // namespace
}  // namespace cortrix
