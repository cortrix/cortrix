#include "cortrix/spc/contextual_enricher.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "cortrix/spc/contextual_error.h"
#include "cortrix/spc/contextual_metrics.h"

namespace cortrix::spc {

namespace {

using json = nlohmann::json;

/// Clamp max_output_tokens to the §6.2 / prompt-injection range [40,200].
int ClampMaxOutputTokens(int v) {
    if (v < kContextualMaxOutputTokensMin) return kContextualMaxOutputTokensMin;
    if (v > kContextualMaxOutputTokensMax) return kContextualMaxOutputTokensMax;
    return v;
}

/// Replace every occurrence of `token` in `s` with `value` (NS-template
/// substitution, §6.2). Simple literal replace; tokens are the {{...}} markers.
void ReplaceAll(std::string& s, const std::string& token, const std::string& value) {
    if (token.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(token, pos)) != std::string::npos) {
        s.replace(pos, token.size(), value);
        pos += value.size();
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// Config resolution (F35 §6.2 three-layer override)
// -----------------------------------------------------------------------------

ContextualRetrievalConfig ResolveContextualConfig(const IGlobalConfig* global) {
    ContextualRetrievalConfig cfg;  // built-in defaults (layer 1)
    if (global == nullptr) return cfg;

    // Layer 2: IGlobalConfig global. Each key is fail-soft — absent / unparseable
    // keeps the built-in default.
    if (auto r = global->GetBool(kContextualEnabledKey); r.ok()) {
        cfg.enabled = r.value();
    }
    if (auto r = global->GetString(kContextualPromptTemplateKey); r.ok()) {
        cfg.prompt_template = r.value();
    }
    if (auto r = global->GetInt(kContextualMaxOutputTokensKey); r.ok()) {
        cfg.max_output_tokens = ClampMaxOutputTokens(r.value());
    }
    if (auto r = global->GetString(kContextualLlmModelKey); r.ok() && !r.value().empty()) {
        cfg.llm_model = r.value();
    }
    if (auto r = global->GetInt(kContextualTimeoutMsKey); r.ok()) {
        cfg.timeout_ms = std::max(r.value(), kContextualTimeoutMsMin);
    }
    return cfg;
}

ContextualRetrievalConfig MergeNsOverride(const ContextualRetrievalConfig& base,
                                          const std::string& ns_metadata_json) {
    ContextualRetrievalConfig cfg = base;
    if (ns_metadata_json.empty()) return cfg;

    json root = json::parse(ns_metadata_json, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) return cfg;  // fail-soft

    // The NS layer may nest under "contextual_retrieval" (§6.2 example) or be flat.
    const json* node = &root;
    if (root.contains("contextual_retrieval") &&
        root["contextual_retrieval"].is_object()) {
        node = &root["contextual_retrieval"];
    }

    if (node->contains("enabled") && (*node)["enabled"].is_boolean()) {
        cfg.enabled = (*node)["enabled"].get<bool>();
    }
    if (node->contains("prompt_template") && (*node)["prompt_template"].is_string()) {
        cfg.prompt_template = (*node)["prompt_template"].get<std::string>();
    }
    if (node->contains("max_output_tokens") &&
        (*node)["max_output_tokens"].is_number_integer()) {
        cfg.max_output_tokens =
            ClampMaxOutputTokens((*node)["max_output_tokens"].get<int>());
    }
    if (node->contains("llm_model") && (*node)["llm_model"].is_string() &&
        !(*node)["llm_model"].get<std::string>().empty()) {
        cfg.llm_model = (*node)["llm_model"].get<std::string>();
    }
    return cfg;
}

// -----------------------------------------------------------------------------
// ContextualRetrievalEnricher
// -----------------------------------------------------------------------------

ContextualRetrievalEnricher::ContextualRetrievalEnricher(
    const ContextualRetrievalConfig& config,
    std::shared_ptr<llm::ILlmClient> llm_client,
    std::shared_ptr<IContextualEmbedder> embedder)
    : config_(config),
      llm_client_(std::move(llm_client)),
      embedder_(std::move(embedder)) {}

ContextualRetrievalEnricher::~ContextualRetrievalEnricher() = default;

bool ContextualRetrievalEnricher::IsAvailable() const {
    // §7.2 L2: a missing LLM client OR a disabled config degrades the whole F35
    // stage (the chunk falls back to its original embedding — L1/L2 equivalent at
    // the stage level). Mirrors F03's null-client / F38's IsAvailable() shape.
    return llm_client_ != nullptr && config_.enabled;
}

std::string ContextualRetrievalEnricher::BuildPrompt(const std::string& chunk_text,
                                                     const DocumentMetadata& doc_meta,
                                                     const ChunkContext& ctx) const {
    // §6.1 Anthropic v1 English default. section_heading is chunk-level
    // (ParsedChunk.section, threaded via the pipeline) — not on the frozen
    // DocumentMetadata; standalone leaves it empty (D3.5 supplies it). prev/next
    // chunk text come from the frozen ChunkContext (empty == doc boundary).
    if (!config_.prompt_template.empty()) {
        // §6.2 NS override: substitute the {{...}} markers into the custom template.
        std::string p = config_.prompt_template;
        ReplaceAll(p, "{{doc_title}}", doc_meta.doc_title);
        ReplaceAll(p, "{{section_heading}}", "");  // chunk-level, D3.5
        ReplaceAll(p, "{{prev_chunk_text}}", ctx.prev_chunk_text);
        ReplaceAll(p, "{{chunk_text}}", chunk_text);
        ReplaceAll(p, "{{next_chunk_text}}", ctx.next_chunk_text);
        return p;
    }

    std::ostringstream os;
    os << "<document_metadata>\n"
       << "  title: " << doc_meta.doc_title << "\n"
       << "  section: " << "" << "\n"  // section_heading: chunk-level (D3.5)
       << "</document_metadata>\n\n"
       << "<previous_chunk>" << ctx.prev_chunk_text << "</previous_chunk>\n\n"
       << "<chunk>" << chunk_text << "</chunk>\n\n"
       << "<next_chunk>" << ctx.next_chunk_text << "</next_chunk>\n\n"
       << "Please give a short succinct context to situate this chunk within\n"
       << "the overall document for the purposes of improving search retrieval of "
          "the chunk.\n"
       << "Answer only with the succinct context and nothing else.\n";
    return os.str();
}

Result<std::string> ContextualRetrievalEnricher::GenerateContextualizedText(
    const std::string& chunk_text, const DocumentMetadata& doc_meta,
    const ChunkContext& ctx) {
    if (llm_client_ == nullptr) {
        // §7.2 L2: no LLM at all → the startup-no-LLM identity (caller maps to skip).
        return ContextualStatus(ContextualErrorCode::kStartupNoLlm,
                                "no LLM client");
    }

    llm::LlmCallConfig call;
    call.model = config_.llm_model;
    call.temperature = 0.0;                       // §6.1 deterministic
    call.max_tokens = config_.max_output_tokens;  // §6.1 (80 default, 40-200)
    call.timeout_ms = config_.timeout_ms;         // §6.1 10s default, configurable

    llm::ChatCompletionResponse resp =
        llm_client_->Chat(BuildPrompt(chunk_text, doc_meta, ctx), call);
    if (!resp.ok()) {
        // §7.3 L3: transport / timeout / rate-limit / network → CX_ERR_F35_LLM_FAILED
        // (transient, retryable; the caller degrades to the original embedding).
        return ContextualStatus(ContextualErrorCode::kLlmFailed, resp.status.message());
    }

    // §8 prompt-injection defense: an output longer than 2 x max_output_tokens
    // (chars as a conservative proxy for token budget — no tokenizer in this seam)
    // is treated as a hostile / runaway generation → permanent reject.
    const size_t guard_limit =
        static_cast<size_t>(config_.max_output_tokens) * 2u;
    if (resp.content.size() > guard_limit) {
        return ContextualStatus(
            ContextualErrorCode::kPromptInjection,
            "output length " + std::to_string(resp.content.size()) +
                " exceeds 2x max_output_tokens " +
                std::to_string(config_.max_output_tokens));
    }
    return resp.content;
}

EnrichResult ContextualRetrievalEnricher::Enrich(const std::string& chunk_text,
                                                 const DocumentMetadata& doc_meta,
                                                 const ChunkContext& ctx) {
    auto& metrics = ContextualRetrievalMetrics::Instance();
    EnrichResult result;
    result.enricher_name = Name();
    result.model_used = config_.llm_model;

    // §7.2 L2: the stage is unavailable (no client / disabled) → skipped_no_llm.
    // The chunk Block is still written upstream with its original embedding.
    if (!IsAvailable()) {
        result.contextualized_status = 3;  // skipped_no_llm
        metrics.RecordChunk(ContextualRetrievalMetrics::ChunkStatus::kSkippedNoLlm);
        return result;
    }

    // 1) LLM contextualized prefix (§6).
    Result<std::string> text = GenerateContextualizedText(chunk_text, doc_meta, ctx);
    if (!text.ok()) {
        // §7.3 transparent degrade: F35 skipped, original embedding kept. The
        // contextualized_status records the failure; error_meta carries the
        // Agent-friendly detail (which CX_ERR_F35_* identity).
        result.contextualized_status = 2;  // failed
        result.error_msg = text.status().message();
        result.error_meta.structured_data =
            std::string("{\"enricher\":\"f35_contextual_retrieval\"}");
        metrics.RecordChunk(ContextualRetrievalMetrics::ChunkStatus::kFailed);
        return result;
    }

    // The contextualized_text = prefix + original chunk (F35 §1.1: the prefix is
    // prepended to the chunk and the whole is re-embedded).
    std::string contextualized = text.value() + "\n" + chunk_text;
    result.contextualized_text = contextualized;

    // 2) Re-embed the contextualized text (§5 double-vector coexistence). A null /
    // failing embedder degrades only the embedding half — the text is still useful
    // (partial result, §7.3).
    if (embedder_ == nullptr) {
        result.contextualized_status = 2;  // failed (embedding half unavailable)
        result.error_msg =
            ContextualStatus(ContextualErrorCode::kEmbeddingFailed, "no embedder")
                .message();
        metrics.RecordChunk(ContextualRetrievalMetrics::ChunkStatus::kFailed);
        return result;
    }
    Result<std::vector<float>> emb = embedder_->Embed(contextualized);
    if (!emb.ok()) {
        result.contextualized_status = 2;  // failed
        result.error_msg =
            ContextualStatus(ContextualErrorCode::kEmbeddingFailed, emb.status().message())
                .message();
        metrics.RecordChunk(ContextualRetrievalMetrics::ChunkStatus::kFailed);
        return result;
    }

    result.contextualized_embedding = std::move(emb.value());
    result.contextualized_status = 1;  // generated
    metrics.RecordChunk(ContextualRetrievalMetrics::ChunkStatus::kGenerated);
    return result;
}

std::vector<EnrichResult> ContextualRetrievalEnricher::EnrichBatch(
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

// -----------------------------------------------------------------------------
// ContextualOnnxEmbedder (production adapter over the frozen OnnxEmbedder)
// -----------------------------------------------------------------------------

ContextualOnnxEmbedder::ContextualOnnxEmbedder(
    std::shared_ptr<cortrix::OnnxEmbedder> embedder)
    : embedder_(std::move(embedder)) {}

Result<std::vector<float>> ContextualOnnxEmbedder::Embed(const std::string& text) {
    if (embedder_ == nullptr) {
        return Status::Unavailable("ContextualOnnxEmbedder: null OnnxEmbedder");
    }
    EmbeddingResult r;
    Status s = embedder_->Embed(text, &r);
    if (!s.ok()) return s;
    return std::move(r.vector);
}

}  // namespace cortrix::spc
