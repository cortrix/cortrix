#include <cstdint>
#include "cortrix/config/config.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

#include "yaml-cpp/yaml.h"
#include "cortrix/logging/logging.h"

namespace cortrix {

namespace {

std::string GetEnv(const char* name) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : "";
}

bool GetEnvBool(const char* name, bool default_val) {
    std::string val = GetEnv(name);
    if (val.empty()) return default_val;
    return val == "true" || val == "1";
}

int GetEnvInt(const char* name, int default_val) {
    std::string val = GetEnv(name);
    if (val.empty()) return default_val;
    try {
        return std::stoi(val);
    } catch (...) {
        return default_val;
    }
}

void LoadFromYaml(const std::string& path, CortrixConfig& config) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const std::exception& e) {
        std::cerr << "[config] WARN: Failed to load YAML file '" << path
                  << "': " << e.what() << ", using defaults\n";
        return;
    }

    // server
    if (root["server"]) {
        auto s = root["server"];
        if (s["host"]) config.server.host = s["host"].as<std::string>();
        if (s["port"]) config.server.port = s["port"].as<int>();
        if (s["thread_count"]) config.server.thread_count = s["thread_count"].as<int>();
        if (s["max_payload_bytes"]) config.server.max_payload_bytes = s["max_payload_bytes"].as<int64_t>();
    }

    // auth
    if (root["auth"]) {
        auto a = root["auth"];
        if (a["enabled"]) config.auth.enabled = a["enabled"].as<bool>();
        if (a["api_keys"]) {
            config.auth.api_keys.clear();
            for (const auto& key_node : a["api_keys"]) {
                ApiKeyConfig kc;
                if (key_node["key_hash"]) kc.key_hash = key_node["key_hash"].as<std::string>();
                if (key_node["tenant_id"]) kc.tenant_id = key_node["tenant_id"].as<std::string>();
                if (key_node["permissions"]) kc.permissions = key_node["permissions"].as<int>();
                if (key_node["expires_at"]) kc.expires_at = key_node["expires_at"].as<int64_t>();
                // allowed_namespaces removed (ARCHITECTURE V6): namespace authz is
                // runtime PermissionService::BatchCheck, not a static per-key list.
                config.auth.api_keys.push_back(std::move(kc));
            }
        }
    }

    // log
    if (root["log"]) {
        auto l = root["log"];
        if (l["level"]) config.log.level = l["level"].as<std::string>();
        if (l["format"]) config.log.format = l["format"].as<std::string>();
        if (l["output"]) config.log.output = l["output"].as<std::string>();
    }

    // namespace
    if (root["namespace"]) {
        auto n = root["namespace"];
        if (n["data_dir"]) config.ns.data_dir = n["data_dir"].as<std::string>();
        if (n["max_active"]) config.ns.max_active = n["max_active"].as<int>();
        if (n["idle_timeout_s"]) config.ns.idle_timeout_s = n["idle_timeout_s"].as<int>();
        if (n["load_timeout_ms_per_ns"])
            config.ns.load_timeout_ms_per_ns = n["load_timeout_ms_per_ns"].as<int64_t>();
    }

    // embedding
    if (root["embedding"]) {
        auto e = root["embedding"];
        if (e["model_path"]) config.embedding.model_path = e["model_path"].as<std::string>();
        if (e["tokenizer_path"]) config.embedding.tokenizer_path = e["tokenizer_path"].as<std::string>();
        if (e["dimension"]) config.embedding.dimension = e["dimension"].as<int>();
        if (e["max_seq_length"]) config.embedding.max_seq_length = e["max_seq_length"].as<int>();
        if (e["gpu_provider"]) config.embedding.gpu_provider = e["gpu_provider"].as<std::string>();
    }

    // Helper lambda: parse a LlmConfig node
    auto parseLlm = [](const YAML::Node& n, LlmConfig& cfg) {
        if (n["provider"]) cfg.provider = n["provider"].as<std::string>();
        if (n["api_key"])  cfg.api_key  = n["api_key"].as<std::string>();
        if (n["model"])    cfg.model    = n["model"].as<std::string>();
        if (n["base_url"]) cfg.base_url = n["base_url"].as<std::string>();
        if (n["timeout_ms"]) cfg.timeout_ms = n["timeout_ms"].as<int>();
        if (n["batch_size"]) cfg.batch_size = n["batch_size"].as<int>();
        if (n["max_tokens"]) cfg.max_tokens = n["max_tokens"].as<int>();
        if (n["hype_questions_per_chunk"])
            cfg.hype_questions_per_chunk = n["hype_questions_per_chunk"].as<int>();
        if (n["ctx_max_output_tokens"])
            cfg.ctx_max_output_tokens = n["ctx_max_output_tokens"].as<int>();
        if (n["ctx_guard_chars_per_token"])
            cfg.ctx_guard_chars_per_token = n["ctx_guard_chars_per_token"].as<int>();
    };

    // semantic_llm — fast/cheap model for intent classification & reranking
    if (root["semantic_llm"]) parseLlm(root["semantic_llm"], config.semantic_llm);

    // vision_llm — multimodal model for OCR image enhancement
    if (root["vision_llm"])   parseLlm(root["vision_llm"],   config.vision_llm);

    // agent_llm — conversational model for RAG/chat (also read by cortrix-agent)
    if (root["agent_llm"])    parseLlm(root["agent_llm"],    config.agent_llm);

    // doc_summary_llm — [F41] ingest-side document summary (async via F42). This
    // parse was missing when the role landed (F42-1 added the field + the main.cpp
    // consumer only) — without it a configured doc_summary_llm: block never reached
    // IsConfigured() and the feature stayed OFF.
    if (root["doc_summary_llm"]) parseLlm(root["doc_summary_llm"], config.doc_summary_llm);

    // enricher_llm — [F03 · gap①] SPC ingest enricher (NER + summary), plan B
    // (F03 §2.7.bis): connection triple here, tuning fields keep the F03 defaults.
    if (root["enricher_llm"])    parseLlm(root["enricher_llm"],    config.enricher_llm);

    // Backward compatibility: old "llm:" key maps to semantic_llm
    if (root["llm"] && !root["semantic_llm"]) {
        parseLlm(root["llm"], config.semantic_llm);
    }

    // vision_llm inherits semantic_llm for any unset fields
    // (provider, api_key, base_url typically the same; only model differs)
    if (config.vision_llm.provider.empty())  config.vision_llm.provider  = config.semantic_llm.provider;
    if (config.vision_llm.api_key.empty())   config.vision_llm.api_key   = config.semantic_llm.api_key;
    if (config.vision_llm.base_url.empty())  config.vision_llm.base_url  = config.semantic_llm.base_url;

    // spc
    if (root["spc"]) {
        auto s = root["spc"];
        if (s["worker_count"]) config.spc.worker_count = s["worker_count"].as<int>();
        if (s["parser_max_concurrent"])
            config.spc.parser_max_concurrent = s["parser_max_concurrent"].as<int>();
        if (s["max_queue_size"]) config.spc.max_queue_size = s["max_queue_size"].as<int>();
        if (s["task_timeout_s"]) config.spc.task_timeout_s = s["task_timeout_s"].as<int>();
        if (s["python_bin"]) config.spc.python_bin = s["python_bin"].as<std::string>();
        if (s["parse_pdf_script"]) config.spc.parse_pdf_script = s["parse_pdf_script"].as<std::string>();
        if (s["parse_word_script"]) config.spc.parse_word_script = s["parse_word_script"].as<std::string>();
        if (s["parser_timeout_s"]) config.spc.parser_timeout_s = s["parser_timeout_s"].as<int>();
        if (s["chunk_size"]) config.spc.chunk_size = s["chunk_size"].as<int>();
        if (s["chunk_overlap"]) config.spc.chunk_overlap = s["chunk_overlap"].as<int>();
        if (s["embedding_batch_size"]) config.spc.embedding_batch_size = s["embedding_batch_size"].as<int>();
        if (s["onnx_intra_threads"]) config.spc.onnx_intra_threads = s["onnx_intra_threads"].as<int>();
        if (s["onnx_inter_threads"]) config.spc.onnx_inter_threads = s["onnx_inter_threads"].as<int>();
        if (s["ocr_script"]) config.spc.ocr_script = s["ocr_script"].as<std::string>();
        if (s["ocr_timeout_s"]) config.spc.ocr_timeout_s = s["ocr_timeout_s"].as<int>();
        if (s["ocr_confidence_threshold"]) config.spc.ocr_confidence_threshold = s["ocr_confidence_threshold"].as<float>();
        if (s["ocr_enable_cls"]) config.spc.ocr_enable_cls = s["ocr_enable_cls"].as<bool>();
        if (s["enable_crash_recovery"]) config.spc.enable_crash_recovery = s["enable_crash_recovery"].as<bool>();
        if (s["vision_llm_script"]) config.spc.vision_llm_script = s["vision_llm_script"].as<std::string>();
        if (s["vision_llm_timeout_s"]) config.spc.vision_llm_timeout_s = s["vision_llm_timeout_s"].as<int>();
        if (s["require_parsers"]) config.spc.require_parsers = s["require_parsers"].as<bool>();
        if (s["enrich_sweep_interval_sec"]) config.spc.enrich_sweep_interval_sec = s["enrich_sweep_interval_sec"].as<int>();
        if (s["enrich_sweep_batch"]) config.spc.enrich_sweep_batch = s["enrich_sweep_batch"].as<int>();
    }

    // upload
    if (root["upload"]) {
        auto u = root["upload"];
        if (u["max_file_size"]) config.upload.max_file_size = u["max_file_size"].as<int64_t>();
        if (u["large_file_threshold"]) config.upload.large_file_threshold = u["large_file_threshold"].as<int64_t>();
        if (u["allowed_mime_types"]) {
            config.upload.allowed_mime_types.clear();
            for (const auto& mt : u["allowed_mime_types"]) {
                config.upload.allowed_mime_types.push_back(mt.as<std::string>());
            }
        }
        if (u["temp_dir"]) config.upload.temp_dir = u["temp_dir"].as<std::string>();
        if (u["max_concurrent_uploads"]) config.upload.max_concurrent_uploads = u["max_concurrent_uploads"].as<int>();
    }

    // watch_dir
    if (root["watch_dir"]) {
        auto w = root["watch_dir"];
        if (w["data_dir"]) config.watch_dir.data_dir = w["data_dir"].as<std::string>();
        if (w["namespace_name"]) config.watch_dir.namespace_name = w["namespace_name"].as<std::string>();
        if (w["watch_enabled"]) config.watch_dir.watch_enabled = w["watch_enabled"].as<bool>();
        if (w["fsevents_latency_s"]) config.watch_dir.fsevents_latency_s = w["fsevents_latency_s"].as<double>();
        if (w["scan_batch_size"]) config.watch_dir.scan_batch_size = w["scan_batch_size"].as<int>();
        if (w["max_concurrent_imports"]) config.watch_dir.max_concurrent_imports = w["max_concurrent_imports"].as<int>();
        if (w["error_threshold"]) config.watch_dir.error_threshold = w["error_threshold"].as<int>();
        if (w["ignore_patterns"]) {
            config.watch_dir.ignore_patterns.clear();
            for (const auto& p : w["ignore_patterns"]) {
                config.watch_dir.ignore_patterns.push_back(p.as<std::string>());
            }
        }
        if (w["allowed_extensions"]) {
            config.watch_dir.allowed_extensions.clear();
            for (const auto& e : w["allowed_extensions"]) {
                config.watch_dir.allowed_extensions.push_back(e.as<std::string>());
            }
        }
    }

    // memory
    if (root["memory"]) {
        auto m = root["memory"];
        if (m["default_ttl_seconds"]) config.memory.default_ttl_seconds = m["default_ttl_seconds"].as<int>();
        if (m["text_to_sql_ttl_seconds"]) config.memory.text_to_sql_ttl_seconds = m["text_to_sql_ttl_seconds"].as<int>();
        if (m["inject_recent_turns"]) config.memory.inject_recent_turns = m["inject_recent_turns"].as<int>();
        if (m["inject_max_tokens"]) config.memory.inject_max_tokens = m["inject_max_tokens"].as<int>();
        if (m["max_sessions_per_namespace"]) config.memory.max_sessions_per_namespace = m["max_sessions_per_namespace"].as<int>();
        if (m["max_interactions_per_session"]) config.memory.max_interactions_per_session = m["max_interactions_per_session"].as<int>();
        if (m["chunk_strategy"]) config.memory.chunk_strategy = m["chunk_strategy"].as<std::string>();
        // [MEM01] memory.decay.* — classified-decay scoring (design § 2.5).
        if (m["decay"]) {
            auto decay = m["decay"];
            if (decay["lambda"]) config.memory.decay_lambda = decay["lambda"].as<double>();
            if (decay["min_score"]) config.memory.decay_min_score = decay["min_score"].as<double>();
        }
    }

    // [F20] security — admin access policy
    if (root["security"]) {
        auto sec = root["security"];
        if (sec["admin_bind"]) config.security.admin_bind = sec["admin_bind"].as<std::string>();
        if (sec["allow_public_admin"]) config.security.allow_public_admin = sec["allow_public_admin"].as<bool>();
    }

    // F02 reranker model dir
    if (root["reranker"]) {
        auto r = root["reranker"];
        if (r["model_dir"]) config.reranker.model_dir = r["model_dir"].as<std::string>();
    }

    // F39 query complexity classifier model dir
    if (root["query_complexity"]) {
        auto qc = root["query_complexity"];
        if (qc["model_dir"]) config.query_complexity.model_dir = qc["model_dir"].as<std::string>();
    }

    // Retrieval candidate-pool sizing (F02 over-fetch cap)
    if (root["retrieval"]) {
        auto rt = root["retrieval"];
        if (rt["candidate_multiplier"])
            config.retrieval.candidate_multiplier = rt["candidate_multiplier"].as<int>();
        if (rt["max_candidates"])
            config.retrieval.max_candidates = rt["max_candidates"].as<int>();
    }

    // [OPEN-2] three-stage GC
    if (root["gc"]) {
        auto g = root["gc"];
        if (g["enabled"]) config.gc.enabled = g["enabled"].as<bool>();
        if (g["soft_delete_retention_days"])
            config.gc.soft_delete_retention_days = g["soft_delete_retention_days"].as<int>();
        if (g["blob_gc_retention_days"])
            config.gc.blob_gc_retention_days = g["blob_gc_retention_days"].as<int>();
        if (g["scan_interval_hours"])
            config.gc.scan_interval_hours = g["scan_interval_hours"].as<int>();
        if (g["max_purge_per_run"])
            config.gc.max_purge_per_run = g["max_purge_per_run"].as<int>();
        if (g["max_run_duration_minutes"])
            config.gc.max_run_duration_minutes = g["max_run_duration_minutes"].as<int>();
        if (g["dry_run"]) config.gc.dry_run = g["dry_run"].as<bool>();
        if (g["immediate_purge_enabled"])
            config.gc.immediate_purge_enabled = g["immediate_purge_enabled"].as<bool>();
    }
}

void ApplyEnvOverrides(CortrixConfig& config) {
    std::string val;

    val = GetEnv("CORTRIX_SERVER_HOST");
    if (!val.empty()) config.server.host = val;

    config.server.port = GetEnvInt("CORTRIX_SERVER_PORT", config.server.port);
    config.server.thread_count = GetEnvInt("CORTRIX_SERVER_THREADS", config.server.thread_count);

    val = GetEnv("CORTRIX_LOG_LEVEL");
    if (!val.empty()) config.log.level = val;

    val = GetEnv("CORTRIX_LOG_FORMAT");
    if (!val.empty()) config.log.format = val;

    val = GetEnv("CORTRIX_LOG_OUTPUT");
    if (!val.empty()) config.log.output = val;

    val = GetEnv("CORTRIX_DATA_DIR");
    if (!val.empty()) config.ns.data_dir = val;

    val = GetEnv("CORTRIX_AUTH_ENABLED");
    if (!val.empty()) config.auth.enabled = GetEnvBool("CORTRIX_AUTH_ENABLED", config.auth.enabled);

    val = GetEnv("CORTRIX_RERANKER_MODEL_DIR");
    if (!val.empty()) config.reranker.model_dir = val;
    val = GetEnv("CORTRIX_QUERY_COMPLEXITY_MODEL_DIR");
    if (!val.empty()) config.query_complexity.model_dir = val;

    config.retrieval.candidate_multiplier =
        GetEnvInt("CORTRIX_RETRIEVAL_CANDIDATE_MULTIPLIER", config.retrieval.candidate_multiplier);
    config.retrieval.max_candidates =
        GetEnvInt("CORTRIX_RETRIEVAL_MAX_CANDIDATES", config.retrieval.max_candidates);

    val = GetEnv("CORTRIX_REQUIRE_PARSERS");
    if (!val.empty()) config.spc.require_parsers = GetEnvBool("CORTRIX_REQUIRE_PARSERS", config.spc.require_parsers);

    // [F20] security — admin access policy
    val = GetEnv("CORTRIX_ADMIN_BIND");
    if (!val.empty()) config.security.admin_bind = val;
    val = GetEnv("CORTRIX_ALLOW_PUBLIC_ADMIN");
    if (!val.empty()) config.security.allow_public_admin = GetEnvBool("CORTRIX_ALLOW_PUBLIC_ADMIN", config.security.allow_public_admin);
}

}  // namespace

CortrixConfig LoadConfig(const std::string& config_path) {
    CortrixConfig config;

    if (!config_path.empty()) {
        std::ifstream f(config_path);
        if (f.good()) {
            LoadFromYaml(config_path, config);
        }
    }

    ApplyEnvOverrides(config);

    return config;
}

std::vector<std::string> ValidateConfig(const CortrixConfig& config) {
    std::vector<std::string> errors;

    // Server validation
    if (config.server.port < 1 || config.server.port > 65535) {
        errors.push_back("server.port must be in range [1, 65535], got " +
                         std::to_string(config.server.port));
    }
    if (config.server.thread_count < 1) {
        errors.push_back("server.thread_count must be >= 1, got " +
                         std::to_string(config.server.thread_count));
    }
    if (config.server.max_payload_bytes <= 0) {
        errors.push_back("server.max_payload_bytes must be > 0, got " +
                         std::to_string(config.server.max_payload_bytes));
    }

    // Namespace validation
    if (config.ns.data_dir.empty()) {
        errors.push_back("namespace.data_dir must not be empty");
    }
    if (config.ns.max_active < 1) {
        errors.push_back("namespace.max_active must be >= 1, got " +
                         std::to_string(config.ns.max_active));
    }
    if (config.ns.idle_timeout_s < 0) {
        errors.push_back("namespace.idle_timeout_s must be >= 0, got " +
                         std::to_string(config.ns.idle_timeout_s));
    }

    // Embedding validation
    if (config.embedding.dimension < 1) {
        errors.push_back("embedding.dimension must be >= 1, got " +
                         std::to_string(config.embedding.dimension));
    }

    // SPC validation (0 = auto-detect after embedder init)
    if (config.spc.worker_count < 0) {
        errors.push_back("spc.worker_count must be >= 0 (0=auto), got " +
                         std::to_string(config.spc.worker_count));
    }
    if (config.spc.chunk_size < 1) {
        errors.push_back("spc.chunk_size must be >= 1, got " +
                         std::to_string(config.spc.chunk_size));
    }
    if (config.spc.chunk_overlap < 0) {
        errors.push_back("spc.chunk_overlap must be >= 0, got " +
                         std::to_string(config.spc.chunk_overlap));
    }
    if (config.spc.chunk_size > 0 && config.spc.chunk_overlap >= config.spc.chunk_size) {
        errors.push_back("spc.chunk_overlap must be < spc.chunk_size (" +
                         std::to_string(config.spc.chunk_size) + "), got " +
                         std::to_string(config.spc.chunk_overlap));
    }

    // Log validation
    {
        const auto& level = config.log.level;
        if (level != "trace" && level != "debug" && level != "info" &&
            level != "warn" && level != "error" && level != "critical" && level != "off") {
            errors.push_back("log.level must be one of {trace,debug,info,warn,error,critical,off}, got '" +
                             level + "'");
        }
    }
    {
        const auto& fmt = config.log.format;
        if (fmt != "json" && fmt != "text") {
            errors.push_back("log.format must be one of {json,text}, got '" + fmt + "'");
        }
    }
    if (config.log.output.empty()) {
        errors.push_back("log.output must not be empty");
    }

    // Upload validation
    if (config.upload.max_file_size <= 0) {
        errors.push_back("upload.max_file_size must be > 0, got " +
                         std::to_string(config.upload.max_file_size));
    }

    // WatchDir validation
    if (config.watch_dir.scan_batch_size < 0) {
        errors.push_back("watch_dir.scan_batch_size must be >= 0, got " +
                         std::to_string(config.watch_dir.scan_batch_size));
    }
    if (config.watch_dir.max_concurrent_imports < 0) {
        errors.push_back("watch_dir.max_concurrent_imports must be >= 0, got " +
                         std::to_string(config.watch_dir.max_concurrent_imports));
    }
    if (config.watch_dir.error_threshold < 0) {
        errors.push_back("watch_dir.error_threshold must be >= 0, got " +
                         std::to_string(config.watch_dir.error_threshold));
    }
    if (config.watch_dir.fsevents_latency_s < 0.0) {
        errors.push_back("watch_dir.fsevents_latency_s must be >= 0.0, got " +
                         std::to_string(config.watch_dir.fsevents_latency_s));
    }

    // SPC timeout validation
    if (config.spc.task_timeout_s < 0) {
        errors.push_back("spc.task_timeout_s must be >= 0, got " +
                         std::to_string(config.spc.task_timeout_s));
    }
    if (config.spc.parser_timeout_s < 0) {
        errors.push_back("spc.parser_timeout_s must be >= 0, got " +
                         std::to_string(config.spc.parser_timeout_s));
    }
    if (config.spc.embedding_batch_size < 1) {
        errors.push_back("spc.embedding_batch_size must be >= 1, got " +
                         std::to_string(config.spc.embedding_batch_size));
    }

    // Memory validation
    if (config.memory.default_ttl_seconds < 0) {
        errors.push_back("memory.default_ttl_seconds must be >= 0, got " +
                         std::to_string(config.memory.default_ttl_seconds));
    }
    if (config.memory.inject_recent_turns < 0) {
        errors.push_back("memory.inject_recent_turns must be >= 0, got " +
                         std::to_string(config.memory.inject_recent_turns));
    }
    if (config.memory.inject_max_tokens < 0) {
        errors.push_back("memory.inject_max_tokens must be >= 0, got " +
                         std::to_string(config.memory.inject_max_tokens));
    }
    if (config.memory.max_sessions_per_namespace < 1) {
        errors.push_back("memory.max_sessions_per_namespace must be >= 1, got " +
                         std::to_string(config.memory.max_sessions_per_namespace));
    }
    if (config.memory.max_interactions_per_session < 1) {
        errors.push_back("memory.max_interactions_per_session must be >= 1, got " +
                         std::to_string(config.memory.max_interactions_per_session));
    }

    // [OPEN-2] GC windows + safety limits
    if (config.gc.soft_delete_retention_days < 0) {
        errors.push_back("gc.soft_delete_retention_days must be >= 0, got " +
                         std::to_string(config.gc.soft_delete_retention_days));
    }
    if (config.gc.blob_gc_retention_days < 0) {
        errors.push_back("gc.blob_gc_retention_days must be >= 0, got " +
                         std::to_string(config.gc.blob_gc_retention_days));
    }
    if (config.gc.scan_interval_hours < 1) {
        errors.push_back("gc.scan_interval_hours must be >= 1, got " +
                         std::to_string(config.gc.scan_interval_hours));
    }
    if (config.gc.max_purge_per_run < 1) {
        errors.push_back("gc.max_purge_per_run must be >= 1, got " +
                         std::to_string(config.gc.max_purge_per_run));
    }
    if (config.gc.max_run_duration_minutes < 1) {
        errors.push_back("gc.max_run_duration_minutes must be >= 1, got " +
                         std::to_string(config.gc.max_run_duration_minutes));
    }

    return errors;
}

}  // namespace cortrix
