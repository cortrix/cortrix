#include "cortrix/doc_summary/doc_summary_generator.h"

#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "cortrix/common/i_global_config.h"
#include "cortrix/doc_summary/doc_summary_error.h"
#include "cortrix/doc_summary/doc_summary_metrics.h"

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
/// cut a multi-byte sequence mid-character). summary_text is the §4.2 200-500
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
    // §9.1 Phase-1 English template v1 (structured JSON output).
    std::ostringstream os;
    os << "System: You are a document summarizer for semantic search.\n"
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
    // §9.2 Map stage — a plain-text partial summary for one chunk group.
    std::ostringstream os;
    os << "System: You are a document summarizer.\n"
       << "User: Summarize this section of the document \"" << doc_title
       << "\" in 2-3 sentences covering its key points.\n\n"
       << "Section content:\n" << group_text << "\n";
    return os.str();
}

std::string DocSummaryGenerator::BuildReducePrompt(
    const std::string& doc_title, const std::vector<std::string>& partials) const {
    // §9.2 Reduce stage — combine the Map partials into the final structured JSON.
    std::ostringstream os;
    os << "System: You are a document summarizer for semantic search.\n"
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
    const std::string& llm_output, int max_chars) {
    json root = json::parse(llm_output, /*cb=*/nullptr, /*allow_exceptions=*/false);
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
    if (llm_client_ == nullptr) {
        return DocSummaryStatus(DocSummaryErrorCode::kLlmTimeout, "no LLM client");
    }
    llm::LlmCallConfig call;
    call.model = config_.llm_model;
    call.temperature = 0.0;
    call.response_format = "json_object";  // §9.1 structured output

    metrics.RecordLlmCall(DocSummaryMetrics::LlmCallStatus::kStarted);
    llm::ChatCompletionResponse resp = llm_client_->Chat(prompt, call);
    if (!resp.ok()) {
        metrics.RecordLlmCall(DocSummaryMetrics::LlmCallStatus::kFailed);
        return DocSummaryStatus(DocSummaryErrorCode::kLlmTimeout, resp.status.message());
    }
    Result<DocSummaryStructured> parsed =
        ParseStructuredOutput(resp.content, config_.max_chars);
    if (!parsed.ok()) {
        metrics.RecordLlmCall(DocSummaryMetrics::LlmCallStatus::kFailed);
        return parsed.status();
    }
    metrics.RecordLlmCall(DocSummaryMetrics::LlmCallStatus::kSuccess);
    return parsed;
}

Result<DocSummaryStructured> DocSummaryGenerator::GenerateSummary(
    const std::vector<store::ChunkRecord>& chunks, const std::string& doc_title,
    bool* used_map_reduce) {
    if (used_map_reduce) *used_map_reduce = false;

    if (static_cast<int>(chunks.size()) <= config_.chunk_threshold) {
        // Short doc: one structured call (§9.2).
        return CallLlmStructured(BuildPrompt(doc_title, JoinChunks(chunks)));
    }

    // Long doc: map-reduce (§9.2). Map each kMapGroupSize-chunk group to a partial.
    if (used_map_reduce) *used_map_reduce = true;
    std::vector<std::string> partials;
    for (size_t start = 0; start < chunks.size(); start += kMapGroupSize) {
        size_t end = std::min(start + static_cast<size_t>(kMapGroupSize), chunks.size());
        std::vector<store::ChunkRecord> group(chunks.begin() + start,
                                              chunks.begin() + end);
        if (llm_client_ == nullptr) {
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
    (void)ns_id;  // ns scoping is applied by the F42 worker / store at D3.5.
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
        // No content to summarize — treat as a permanent doc-too-large/empty case
        // (the F42 worker marks status=failed; no retry helps an empty doc).
        result.error = MakeDocSummaryError(
            DocSummaryErrorCode::kDocTooLarge,
            {{"doc_id", doc_id}, {"doc_size_bytes", 0}, {"max_allowed_bytes", 0}},
            "document has no chunks to summarize");
        return result;
    }

    // doc_title: best-effort from the first chunk's content is not reliable, so
    // standalone leaves it empty unless the caller threads it (real DocumentMetadata
    // join is D3.5 pipeline wiring). The prompt simply omits a blank title.
    bool used_map_reduce = false;
    Result<DocSummaryStructured> summary =
        GenerateSummary(chunks, /*doc_title=*/"", &used_map_reduce);
    if (!summary.ok()) {
        // Re-inflate the Agent-friendly body from the CX_ERR_F41_* token carried in
        // the Status message (the registry has the canonical category/retryable).
        const std::string& msg = summary.status().message();
        DocSummaryErrorCode code = DocSummaryErrorCode::kLlmInvalidOutput;
        if (msg.find("CX_ERR_F41_LLM_TIMEOUT") != std::string::npos)
            code = DocSummaryErrorCode::kLlmTimeout;
        else if (msg.find("CX_ERR_F41_LLM_BUDGET_EXCEEDED") != std::string::npos)
            code = DocSummaryErrorCode::kLlmBudgetExceeded;
        result.error = MakeDocSummaryError(code, {{"doc_id", doc_id}}, msg);
        return result;
    }

    result.success = true;
    result.summary = std::move(summary.value());
    result.is_chunked = used_map_reduce;
    // embedding stays empty — the OnnxEmbedder re-embed + doc_summary Block write +
    // P-HNSW index are D3.5 pipeline wiring.
    DocSummaryMetrics::Instance().AddSummariesGenerated(1);
    return result;
}

}  // namespace cortrix::doc_summary
