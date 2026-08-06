#include "cortrix/doc_summary/doc_summary_generator.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "cortrix/common/json_contract.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/doc_summary/doc_summary_error.h"
#include "cortrix/doc_summary/doc_summary_metrics.h"
#include "cortrix/logging/logging.h"

namespace cortrix::doc_summary {

namespace {

using json = nlohmann::json;

/// Join chunk text with blank-line separators (the LLM prompt input).
std::string JoinChunks(const std::vector<store::ChunkRecord>& chunks) {
    std::ostringstream os;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (i) os << "\n\n";
        os << chunks[i].content;
    }
    return os.str();
}

/// UTF-8-safe truncation to at most `max_chars` Unicode code points (so we never
/// cut a multi-byte sequence mid-character). summary_text is the 200-500
/// char field; the LLM is asked to stay in range but we hard-cap defensively.
std::string TruncateUtf8(const std::string& s, int max_chars) {
    if (max_chars <= 0) return std::string();
    int count = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((c & 0x80) == 0x00) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (i + step > s.size()) break;  // truncated trailing bytes — stop clean
        ++count;
        if (count > max_chars) return s.substr(0, i);
        i += step;
    }
    return s;
}

std::vector<std::string> ToStringArray(const json& node, const std::string& key) {
    std::vector<std::string> out;
    if (node.contains(key) && node[key].is_array()) {
        for (const auto& e : node[key]) {
            if (e.is_string()) out.push_back(e.get<std::string>());
        }
    }
    return out;
}

// Escape machinery lives in common/json_contract.h since QA 2026-07-12 (F-10)
// so the enricher batch parser shares the exact repair this file pioneered.
json ParseJsonObjectWithLlmStringEscapeRepair(const std::string& value) {
    json root = json::parse(value, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (!root.is_discarded() && root.is_object()) return root;

    const std::string repaired = common::EscapeInvalidJsonStringBackslashes(value);
    if (repaired == value) return root;
    return json::parse(repaired, /*cb=*/nullptr, /*allow_exceptions=*/false);
}

std::string TruncateForDiagnostics(const std::string& value, size_t max_bytes) {
    if (value.size() <= max_bytes) return value;
    return value.substr(0, max_bytes) + "...[truncated]";
}

json BuildLlmInvalidOutputDiagnostics(const llm::ChatCompletionResponse& resp,
                                      const std::string& parse_error) {
    return json{
        {"raw_output_preview", TruncateForDiagnostics(resp.content, 4000)},
        {"raw_output_length", resp.content.size()},
        {"parse_error", parse_error},
        {"llm_content_source", resp.content_source},
        {"llm_model", resp.model},
        {"llm_finish_reason", resp.finish_reason},
        {"llm_content_length", resp.content_length},
        {"llm_reasoning_content_length", resp.reasoning_content_length},
        {"llm_prompt_tokens", resp.prompt_tokens},
        {"llm_completion_tokens", resp.completion_tokens},
    };
}

json BuildLlmTimeoutDiagnostics(const std::string& error_message) {
    return json{
        {"attempt_count", 1},
        {"last_error_message", error_message},
    };
}

std::string DescribeLlmResponseForError(const llm::ChatCompletionResponse& resp,
                                        const std::string& parse_error) {
    std::ostringstream os;
    os << parse_error
       << "; content_source=" << resp.content_source
       << "; model=" << resp.model
       << "; finish_reason=" << resp.finish_reason
       << "; content_length=" << resp.content_length
       << "; reasoning_content_length=" << resp.reasoning_content_length
       << "; prompt_tokens=" << resp.prompt_tokens
       << "; completion_tokens=" << resp.completion_tokens;
    return os.str();
}

bool ModelPrefersJsonObjectResponseFormat(std::string model) {
    std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return model.find("deepseek") != std::string::npos;
}

bool ModelSupportsThinkingControl(std::string model) {
    std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return model.find("deepseek") != std::string::npos;
}

json StructuredDataForCode(DocSummaryErrorCode code,
                           const json& diagnostics,
                           const std::string& doc_id,
                           const std::string& fallback_message) {
    json sd = diagnostics.is_object() ? diagnostics : json::object();
    sd["doc_id"] = doc_id;
    switch (code) {
        case DocSummaryErrorCode::kLlmTimeout:
            if (!sd.contains("attempt_count")) sd["attempt_count"] = 1;
            if (!sd.contains("last_error_message")) sd["last_error_message"] = fallback_message;
            break;
        case DocSummaryErrorCode::kLlmInvalidOutput:
            if (!sd.contains("raw_output_preview")) sd["raw_output_preview"] = "";
            if (!sd.contains("parse_error")) sd["parse_error"] = fallback_message;
            break;
        case DocSummaryErrorCode::kLlmBudgetExceeded:
            if (!sd.contains("budget_remaining_usd")) sd["budget_remaining_usd"] = nullptr;
            if (!sd.contains("budget_reset_at")) sd["budget_reset_at"] = nullptr;
            break;
        case DocSummaryErrorCode::kDocTooLarge:
            if (!sd.contains("doc_size_bytes")) sd["doc_size_bytes"] = 0;
            if (!sd.contains("max_allowed_bytes")) sd["max_allowed_bytes"] = 0;
            break;
        case DocSummaryErrorCode::kSchemaVersionMismatch:
            if (!sd.contains("expected_version")) sd["expected_version"] = nullptr;
            if (!sd.contains("actual_version")) sd["actual_version"] = nullptr;
            break;
        case DocSummaryErrorCode::kFallbackFailed:
            if (!sd.contains("fallback_type")) sd["fallback_type"] = nullptr;
            if (!sd.contains("error_message")) sd["error_message"] = fallback_message;
            break;
        case DocSummaryErrorCode::kFts5FallbackFailed:
            if (!sd.contains("fts5_query")) sd["fts5_query"] = nullptr;
            if (!sd.contains("error_message")) sd["error_message"] = fallback_message;
            break;
    }
    return sd;
}

}  // namespace

DocSummaryConfig ResolveDocSummaryConfig(const cortrix::IGlobalConfig* global) {
    DocSummaryConfig cfg;  // built-in defaults
    if (global == nullptr) return cfg;
    if (auto r = global->GetInt(kMaxCharsKey); r.ok() && r.value() > 0) {
        cfg.max_chars = r.value();
    }
    if (auto r = global->GetInt(kChunkThresholdKey); r.ok() && r.value() > 0) {
        cfg.chunk_threshold = r.value();
    }
    if (auto r = global->GetString(kPromptVersionKey); r.ok() && !r.value().empty()) {
        cfg.prompt_version = r.value();
    }
    if (auto r = global->GetBool(kFts5FallbackEnabledKey); r.ok()) {
        cfg.fts5_fallback_enabled = r.value();
    }
    return cfg;
}

DocSummaryGenerator::DocSummaryGenerator(const DocSummaryConfig& config,
                                         std::shared_ptr<llm::ILlmClient> llm_client,
                                         std::shared_ptr<store::ChunkStore> chunk_store)
    : config_(config),
      llm_client_(std::move(llm_client)),
      chunk_store_(std::move(chunk_store)) {}

DocSummaryGenerator::~DocSummaryGenerator() = default;

std::string DocSummaryGenerator::BuildPrompt(
    const std::string& doc_title, const std::string& chunks_concatenated) const {
    // Phase-1 English template v1 (structured JSON output).
    std::ostringstream os;
    os << "System: You are a document summarizer for semantic search.\n"
       << "Return only one valid JSON object. Do not include analysis, reasoning, "
          "markdown fences, bullets, or explanatory text before or after the JSON.\n"
       << "User: Generate a summary of the following document with structured JSON "
          "output.\n\n"
       << "Requirements:\n"
       << "- summary_text: 200-500 characters covering main topics, key entities, "
          "conclusions. Natural sentences (no bullet points)\n"
       << "- keywords: 3-8 most important keywords/phrases\n"
       << "- topics: 1-3 high-level topic categories (e.g., \"Technical "
          "Documentation\", \"Financial Report\")\n"
       << "- one_liner: One-sentence summary (max 50 characters)\n"
       << "- Match the document's language\n\n"
       << "Document title: " << doc_title << "\n"
       << "Document content: " << chunks_concatenated << "\n\n"
       << "Output JSON:\n"
       << "{\n  \"summary_text\": \"...\",\n  \"keywords\": [\"...\"],\n"
       << "  \"topics\": [\"...\"],\n  \"one_liner\": \"...\"\n}\n";
    return os.str();
}

std::string DocSummaryGenerator::BuildGroupSummaryPrompt(
    const std::string& doc_title, const std::string& group_text) const {
    // Map stage — a plain-text partial summary for one chunk group.
    std::ostringstream os;
    os << "System: You are a document summarizer.\n"
       << "User: Summarize this section of the document \"" << doc_title
       << "\" in 2-3 sentences covering its key points.\n\n"
       << "Section content:\n" << group_text << "\n";
    return os.str();
}

std::string DocSummaryGenerator::BuildReducePrompt(
    const std::string& doc_title, const std::vector<std::string>& partials) const {
    // Reduce stage — combine the Map partials into the final structured JSON.
    std::ostringstream os;
    os << "System: You are a document summarizer for semantic search.\n"
       << "Return only one valid JSON object. Do not include analysis, reasoning, "
          "markdown fences, bullets, or explanatory text before or after the JSON.\n"
       << "User: The following are section summaries of one document. Combine them "
          "into a single structured JSON summary of the whole document.\n\n"
       << "Requirements:\n"
       << "- summary_text: 200-500 characters, natural sentences\n"
       << "- keywords: 3-8 keywords/phrases\n"
       << "- topics: 1-3 topic categories\n"
       << "- one_liner: One-sentence summary (max 50 characters)\n\n"
       << "Document title: " << doc_title << "\n"
       << "Section summaries:\n";
    for (size_t i = 0; i < partials.size(); ++i) {
        os << "[" << (i + 1) << "] " << partials[i] << "\n";
    }
    os << "\nOutput JSON:\n{\n  \"summary_text\": \"...\",\n  \"keywords\": "
          "[\"...\"],\n  \"topics\": [\"...\"],\n  \"one_liner\": \"...\"\n}\n";
    return os.str();
}

Result<DocSummaryStructured> DocSummaryGenerator::ParseStructuredOutput(
    const std::string& llm_output, int max_chars, std::string* parse_repair) {
    if (parse_repair) parse_repair->clear();
    const std::string normalized_output = common::UnwrapCompleteJsonFence(llm_output);
    json root = json::parse(normalized_output, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        root = ParseJsonObjectWithLlmStringEscapeRepair(normalized_output);
        if (!root.is_discarded() && root.is_object()) {
            if (parse_repair) *parse_repair = "invalid_string_escape";
        }
    }
    if (root.is_discarded()) {
        // Second chance (deep-QA 2026-07-10, recorded method change): providers
        // occasionally wrap the object in prose or an UNCLOSED fence — both
        // rightly refused by the strict path, both observed live as
        // CX_ERR_DOCSUMMARY_LLM_INVALID_OUTPUT with DeepSeek-V4-Flash mid-drain. A
        // balanced-brace extraction rescues exactly that WRAPPER NOISE. It runs
        // only when the whole body failed to parse: output that parses to a
        // wrong structure (top-level array/string) is a real contract
        // violation and still rejects, as does truncated/unbalanced JSON.
        const std::string extracted =
            common::ExtractFirstBalancedJsonObject(llm_output);
        if (!extracted.empty()) {
            root = json::parse(extracted, /*cb=*/nullptr, /*allow_exceptions=*/false);
            bool repaired_escape = false;
            if (root.is_discarded()) {
                root = ParseJsonObjectWithLlmStringEscapeRepair(extracted);
                repaired_escape = !root.is_discarded();
            }
            if (!root.is_discarded() && root.is_object()) {
                if (parse_repair) {
                    *parse_repair = repaired_escape
                                        ? "balanced_extract_invalid_string_escape"
                                        : "balanced_extract";
                }
            }
        }
    }
    if (root.is_discarded() || !root.is_object()) {
        return DocSummaryStatus(DocSummaryErrorCode::kLlmInvalidOutput,
                                "output is not a JSON object");
    }
    if (!root.contains("summary_text") || !root["summary_text"].is_string()) {
        return DocSummaryStatus(DocSummaryErrorCode::kLlmInvalidOutput,
                                "missing/invalid summary_text");
    }
    DocSummaryStructured s;
    s.summary_text = TruncateUtf8(root["summary_text"].get<std::string>(), max_chars);
    s.keywords = ToStringArray(root, "keywords");
    s.topics = ToStringArray(root, "topics");
    if (root.contains("one_liner") && root["one_liner"].is_string()) {
        s.one_liner = root["one_liner"].get<std::string>();
    }
    return s;
}

Result<DocSummaryStructured> DocSummaryGenerator::CallLlmStructured(
    const std::string& prompt) {
    auto& metrics = DocSummaryMetrics::Instance();
    last_llm_failure_structured_data_ = json::object();
    if (llm_client_ == nullptr) {
        last_llm_failure_structured_data_ = BuildLlmTimeoutDiagnostics("no LLM client");
        return DocSummaryStatus(DocSummaryErrorCode::kLlmTimeout, "no LLM client");
    }
    llm::LlmCallConfig call;
    call.model = config_.llm_model;
    call.temperature = 0.0;
    call.max_tokens = 4096;
    call.allow_reasoning_content_fallback = false;
    // GLM-5.2's OpenAI-compatible JSON response_format can inject a conflicting
    // provider-side instruction to end with a markdown fence. Doc-summary owns a strict
    // JSON-only prompt and parser, so avoid provider-specific wrapper prompts there.
    // DeepSeek's reasoning models need the JSON object contract to put the final
    // answer in message.content instead of running until length in reasoning_content.
    if (ModelPrefersJsonObjectResponseFormat(call.model)) {
        call.response_format = "json_object";
    } else {
        call.response_format.clear();
    }
    if (ModelSupportsThinkingControl(call.model)) {
        // Doc-summary requires the final structured object in message.content. DeepSeek
        // thinking mode can spend the full token budget in reasoning_content and
        // leave message.content empty on real documents.
        call.thinking_type = "disabled";
    }

    metrics.RecordLlmCall(DocSummaryMetrics::LlmCallStatus::kStarted);
    llm::ChatCompletionResponse resp = llm_client_->Chat(prompt, call);
    if (!resp.ok()) {
        metrics.RecordLlmCall(DocSummaryMetrics::LlmCallStatus::kFailed);
        last_llm_failure_structured_data_ =
            BuildLlmTimeoutDiagnostics(resp.status.message());
        return DocSummaryStatus(DocSummaryErrorCode::kLlmTimeout, resp.status.message());
    }
    std::string parse_repair;
    Result<DocSummaryStructured> parsed =
        ParseStructuredOutput(resp.content, config_.max_chars, &parse_repair);
    if (!parsed.ok()) {
        metrics.RecordLlmCall(DocSummaryMetrics::LlmCallStatus::kFailed);
        last_llm_failure_structured_data_ =
            BuildLlmInvalidOutputDiagnostics(resp, parsed.status().message());
        return DocSummaryStatus(
            DocSummaryErrorCode::kLlmInvalidOutput,
            DescribeLlmResponseForError(resp, parsed.status().message()));
    }
    if (!parse_repair.empty()) {
        // Observable-by-design: a repair is a provider-contract deviation worth
        // tracking per model, never a silent normal path.
        CORTRIX_LOG_WARN("doc_summary",
                         "structured output accepted via parse repair '{}' "
                         "(model={} content_length={})",
                         parse_repair, resp.model, resp.content_length);
    }
    metrics.RecordLlmCall(DocSummaryMetrics::LlmCallStatus::kSuccess);
    last_llm_failure_structured_data_ = json::object();
    return parsed;
}

Result<DocSummaryStructured> DocSummaryGenerator::GenerateSummary(
    const std::vector<store::ChunkRecord>& chunks, const std::string& doc_title,
    bool* used_map_reduce) {
    if (used_map_reduce) *used_map_reduce = false;
    last_llm_failure_structured_data_ = json::object();

    if (static_cast<int>(chunks.size()) <= config_.chunk_threshold) {
        // Short doc: one structured call.
        return CallLlmStructured(BuildPrompt(doc_title, JoinChunks(chunks)));
    }

    // Long doc: map-reduce. Map each kMapGroupSize-chunk group to a partial.
    if (used_map_reduce) *used_map_reduce = true;
    std::vector<std::string> partials;
    for (size_t start = 0; start < chunks.size(); start += kMapGroupSize) {
        size_t end = std::min(start + static_cast<size_t>(kMapGroupSize), chunks.size());
        std::vector<store::ChunkRecord> group(chunks.begin() + start,
                                              chunks.begin() + end);
        if (llm_client_ == nullptr) {
            last_llm_failure_structured_data_ = BuildLlmTimeoutDiagnostics("no LLM client");
            return DocSummaryStatus(DocSummaryErrorCode::kLlmTimeout, "no LLM client");
        }
        llm::LlmCallConfig call;
        call.model = config_.llm_model;
        call.temperature = 0.0;
        DocSummaryMetrics::Instance().RecordLlmCall(
            DocSummaryMetrics::LlmCallStatus::kStarted);
        llm::ChatCompletionResponse resp =
            llm_client_->Chat(BuildGroupSummaryPrompt(doc_title, JoinChunks(group)), call);
        if (!resp.ok()) {
            DocSummaryMetrics::Instance().RecordLlmCall(
                DocSummaryMetrics::LlmCallStatus::kFailed);
            last_llm_failure_structured_data_ =
                BuildLlmTimeoutDiagnostics("map stage: " + resp.status.message());
            return DocSummaryStatus(DocSummaryErrorCode::kLlmTimeout,
                                    "map stage: " + resp.status.message());
        }
        DocSummaryMetrics::Instance().RecordLlmCall(
            DocSummaryMetrics::LlmCallStatus::kSuccess);
        partials.push_back(resp.content);
    }
    // Reduce: combine partials into the final structured summary.
    return CallLlmStructured(BuildReducePrompt(doc_title, partials));
}

GenerationResult DocSummaryGenerator::Generate(const std::string& doc_id,
                                               const std::string& ns_id) {
    (void)ns_id;  // ns scoping is applied by the async worker / store.
    GenerationResult result;
    if (chunk_store_ == nullptr) {
        result.error = MakeDocSummaryError(
            DocSummaryErrorCode::kLlmInvalidOutput,
            {{"doc_id", doc_id}, {"raw_output_preview", ""}, {"parse_error", "no chunk store"}},
            "no chunk store");
        return result;
    }
    std::vector<store::ChunkRecord> chunks = chunk_store_->GetChunksByDocId(doc_id);
    if (chunks.empty()) {
        // An explicit doc_id with no chunks is a valid empty document. Complete the
        // downstream task without calling the LLM or writing a summary block so the
        // task lifecycle count still matches the imported document count.
        result.success = true;
        result.no_summary_content = true;
        return result;
    }

    // doc_title: best-effort from the first chunk's content is not reliable, so
    // standalone leaves it empty unless the caller threads it (real DocumentMetadata
    // join is integration pipeline wiring). The prompt simply omits a blank title.
    bool used_map_reduce = false;
    Result<DocSummaryStructured> summary =
        GenerateSummary(chunks, /*doc_title=*/"", &used_map_reduce);
    if (!summary.ok()) {
        // Re-inflate the Agent-friendly body from the CX_ERR_DOCSUMMARY_* token carried in
        // the Status message (the registry has the canonical category/retryable).
        const std::string& msg = summary.status().message();
        DocSummaryErrorCode code = DocSummaryErrorCode::kLlmInvalidOutput;
        if (msg.find("CX_ERR_DOCSUMMARY_LLM_TIMEOUT") != std::string::npos)
            code = DocSummaryErrorCode::kLlmTimeout;
        else if (msg.find("CX_ERR_DOCSUMMARY_LLM_BUDGET_EXCEEDED") != std::string::npos)
            code = DocSummaryErrorCode::kLlmBudgetExceeded;
        result.error = MakeDocSummaryError(
            code,
            StructuredDataForCode(code, last_llm_failure_structured_data_, doc_id, msg),
            msg);
        return result;
    }

    result.success = true;
    result.summary = std::move(summary.value());
    result.is_chunked = used_map_reduce;
    // embedding stays empty — the OnnxEmbedder re-embed + doc_summary Block write +
    // P-HNSW index are integration pipeline wiring.
    DocSummaryMetrics::Instance().AddSummariesGenerated(1);
    return result;
}

}  // namespace cortrix::doc_summary
