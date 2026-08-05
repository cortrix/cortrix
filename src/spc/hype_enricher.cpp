#include "cortrix/spc/hype_enricher.h"

#include <sstream>

#include "cortrix/spc/hype_error.h"

namespace cortrix::spc {

namespace {
// Split LLM output into trimmed, non-empty lines (one question per line, §6.2).
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        // trim leading/trailing whitespace + CR.
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        out.push_back(line.substr(b, e - b + 1));
    }
    return out;
}
}  // namespace

HyPEEnricher::HyPEEnricher(const HyPEConfig& config,
                           std::shared_ptr<llm::ILlmClient> llm_client,
                           std::shared_ptr<store::ParentChunkStore> parent_store)
    : config_(config),
      llm_client_(std::move(llm_client)),
      parent_store_(std::move(parent_store)) {}

HyPEEnricher::~HyPEEnricher() = default;

bool HyPEEnricher::IsAvailable() const {
    // A missing LLM client degrades the whole HyPE stage (like the enricher's
    // null-client path). K must be in range to be meaningful.
    return llm_client_ != nullptr && config_.questions_per_chunk >= kHypeQuestionsMin;
}

std::string HyPEEnricher::BuildPrompt(const std::string& chunk_text,
                                      const std::string& parent_text,
                                      const DocumentMetadata& doc_meta,
                                      int k) const {
    // §6.1 Phase-1 English template v1. parent_text is optional (reconcile 2):
    // when empty the "Parent context" section is OMITTED entirely (a blank
    // context block would just waste tokens / confuse the model).
    std::ostringstream os;
    os << "System: You are a hypothetical question generator for semantic search.\n"
       << "User: Given the document chunk below, generate exactly " << k
       << " questions this chunk could answer.\n\n"
       << "Requirements:\n"
       << "- One question per line, no numbering, no prefix\n"
       << "- Self-contained (no external pronouns)\n"
       << "- Match the language of the chunk content\n"
       << "- Diverse coverage (different aspects)\n\n";
    if (!parent_text.empty()) {
        os << "Parent context (for understanding chunk semantics):\n"
           << parent_text << "\n\n";
    }
    os << "Chunk:\n" << chunk_text << "\n\n";
    if (!doc_meta.doc_title.empty()) {
        os << "Document: " << doc_meta.doc_title << "\n";
    }
    os << "Output exactly " << k << " lines, each one question.\n";
    return os.str();
}

Result<std::vector<std::string>> HyPEEnricher::ParseQuestions(
    const std::string& llm_output, int expected_k) {
    std::vector<std::string> questions = SplitLines(llm_output);
    if (questions.empty()) {
        // Entirely empty / whitespace output → invalid LLM output (§7).
        return HypeStatus(HypeErrorCode::kLlmInvalidOutput,
                          "empty output, expected " + std::to_string(expected_k) +
                              " questions");
    }
    if (static_cast<int>(questions.size()) != expected_k) {
        // §6.2: count mismatch → parse failure (transient, retryable).
        return HypeStatus(HypeErrorCode::kQuestionParseFailed,
                          "expected " + std::to_string(expected_k) +
                              " questions, got " +
                              std::to_string(questions.size()));
    }
    return questions;
}

Result<std::string> HyPEEnricher::ResolveParentText(const std::string& parent_id) {
    // Optional context (reconcile 2): no store or no id → empty context, not error.
    if (parent_store_ == nullptr || parent_id.empty()) {
        return std::string{};
    }
    Result<cortrix::chunker::ParentChunk> r = parent_store_->GetParent(parent_id);
    if (!r.ok()) {
        // Surface the store failure as the HyPE parent-not-found identity. The
        // caller decides fatal-vs-degrade.
        return HypeStatus(HypeErrorCode::kParentNotFound,
                          "parent_id=" + parent_id + " (" + r.status().message() +
                              ")");
    }
    return r.value().parent_text;  // parent_text field name
}

Result<std::vector<HypeQuestion>> HyPEEnricher::GenerateHypeQuestions(
    const std::string& chunk_text, const std::string& parent_text,
    const DocumentMetadata& doc_meta, const std::string& source_child_id,
    const std::string& source_parent_id) {
    const int k = config_.questions_per_chunk;
    if (llm_client_ == nullptr) {
        return HypeStatus(HypeErrorCode::kLlmTimeout, "no LLM client");
    }

    llm::LlmCallConfig call;
    call.model = config_.llm_model;
    if (config_.timeout_ms > 0) {
        call.timeout_ms = config_.timeout_ms;  // 0 = keep the client default
    }
    llm::ChatCompletionResponse resp =
        llm_client_->Chat(BuildPrompt(chunk_text, parent_text, doc_meta, k), call);
    if (!resp.ok()) {
        // LLM transport/timeout failure → CX_ERR_F38_LLM_TIMEOUT (transparent
        // degrade upstream).
        return HypeStatus(HypeErrorCode::kLlmTimeout, resp.status.message());
    }

    // §6.2 strict parse: exactly K lines, else CX_ERR_F38_QUESTION_PARSE_FAILED /
    // CX_ERR_F38_LLM_INVALID_OUTPUT (S2).
    Result<std::vector<std::string>> parsed = ParseQuestions(resp.content, k);
    if (!parsed.ok()) {
        return parsed.status();
    }
    std::vector<HypeQuestion> questions;
    questions.reserve(parsed.value().size());
    for (int i = 0; i < static_cast<int>(parsed.value().size()); ++i) {
        HypeQuestion q;
        q.question_text = parsed.value()[i];
        q.question_index = i;
        q.source_child_id = source_child_id;
        q.source_parent_id = source_parent_id;
        // embedding filled by the pipeline via OnnxEmbedder (S3).
        questions.push_back(std::move(q));
    }
    return questions;
}

EnrichResult HyPEEnricher::Enrich(const std::string& chunk_text,
                                  const DocumentMetadata& doc_meta,
                                  const ChunkContext& ctx) {
    (void)ctx;  // parent_id is not in the frozen 3-param signature (reconcile 2);
                // standalone Enrich() runs HyPE with no parent context. The real
                // pipeline resolves parent_text + writes hype_question Blocks → D3.5.
    EnrichResult result;
    result.enricher_name = Name();
    result.model_used = config_.llm_model;
    result.prompt_version = config_.prompt_version;

    // reconcile 1: the frozen EnrichResult has no hype_questions slot, so Enrich()
    // only reports the HyPE outcome status. Questions are produced by
    // GenerateHypeQuestions(); the pipeline calls that + builds Blocks at D3.5.
    Result<std::vector<HypeQuestion>> q = GenerateHypeQuestions(
        chunk_text, /*parent_text=*/"", doc_meta, /*source_child_id=*/"",
        /*source_parent_id=*/"");
    if (!q.ok()) {
        // Transparent degrade: HyPE skipped, chunk Block still written
        // upstream. error_meta carries the Agent-friendly detail (S5 fills the
        // canonical category/retryable from the registry).
        result.status = 1;  // non-zero = failed (S5 maps to the EnricherErrorCode/§7)
        result.error_msg = q.status().message();
        result.error_meta.structured_data =
            std::string("{\"enricher\":\"f38_hype\"}");
    }
    return result;
}

std::vector<EnrichResult> HyPEEnricher::EnrichBatch(
    const std::vector<ChunkContext>& contexts) {
    std::vector<EnrichResult> out;
    out.reserve(contexts.size());
    for (const auto& ctx : contexts) {
        const DocumentMetadata empty_meta{};
        const DocumentMetadata& meta =
            ctx.doc_metadata ? *ctx.doc_metadata : empty_meta;
        out.push_back(Enrich(ctx.chunk_text, meta, ctx));
    }
    return out;
}

}  // namespace cortrix::spc
