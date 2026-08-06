#include "cortrix/memory/contradiction_detector.h"

#include <nlohmann/json.hpp>

#include "cortrix/memory/mem02_error.h"
#include "cortrix/memory/prompt_templates.h"

namespace cortrix::memory {

using Judgment = MemoryExtractor::Judgment;

ContradictionDetector::ContradictionDetector(std::shared_ptr<llm::ILlmClient> llm,
                                             std::string model,
                                             int timeout_ms)
    : llm_(std::move(llm)), model_(std::move(model)), timeout_ms_(timeout_ms) {}

Result<Judgment> ContradictionDetector::ParseJudgmentJson(const std::string& llm_output) {
    nlohmann::json j = nlohmann::json::parse(llm_output, /*cb=*/nullptr,
                                             /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return Mem02Status(Mem02ErrorCode::kExtractInvalidOutput,
                           "contradiction judge output is not a JSON object");
    }
    Judgment out;
    // is_contradiction (required, bool — accept bool or 0/1)
    if (j.contains("is_contradiction")) {
        const auto& v = j["is_contradiction"];
        if (v.is_boolean()) {
            out.is_contradiction = v.get<bool>();
        } else if (v.is_number()) {
            out.is_contradiction = v.get<double>() != 0.0;
        } else {
            return Mem02Status(Mem02ErrorCode::kExtractInvalidOutput,
                               "is_contradiction is not a boolean");
        }
    } else {
        return Mem02Status(Mem02ErrorCode::kExtractInvalidOutput,
                           "missing is_contradiction");
    }
    if (j.contains("reason") && j["reason"].is_string()) {
        out.reason = j["reason"].get<std::string>();
    }
    if (j.contains("confidence") && j["confidence"].is_number()) {
        out.confidence = j["confidence"].get<double>();
        if (out.confidence < 0.0) out.confidence = 0.0;
        if (out.confidence > 1.0) out.confidence = 1.0;
    }
    return out;
}

Result<Judgment> ContradictionDetector::Judge(const std::string& new_content,
                                              const std::string& old_content,
                                              const observability::TraceContext* ctx) {
    (void)ctx;  // W3C trace propagation to the LLM transport is integration wiring

    if (!llm_) {
        return Mem02Status(Mem02ErrorCode::kLlmDisabled, "no LLM client for judge");
    }

    // Build the D5 judgment prompt (language auto-picked from the new fact text).
    const char* tmpl = ContradictionPromptTemplate(PromptLang::kAuto, new_content);
    std::string prompt = RenderPrompt(tmpl, kPlaceholderNewFact, new_content);
    prompt = RenderPrompt(prompt, kPlaceholderOldFact, old_content);

    llm::LlmCallConfig cfg;
    cfg.model = model_;
    cfg.timeout_ms = timeout_ms_;
    cfg.response_format = "json_object";

    llm::ChatCompletionResponse resp = llm_->Chat(prompt, cfg);
    if (!resp.ok()) {
        // Map the transport error to the MEM02 judge fault family. A timeout
        // (kUnavailable from the client) → LLM_TIMEOUT; otherwise treat as ambiguous
        // (the judge could not produce a usable verdict).
        if (resp.status.code() == StatusCode::kUnavailable) {
            return Mem02Status(Mem02ErrorCode::kExtractLlmTimeout,
                               "contradiction judge LLM call failed/timed out");
        }
        return Mem02Status(Mem02ErrorCode::kContradictionAmbiguous,
                           "contradiction judge LLM error: " + resp.status.message());
    }

    return ParseJudgmentJson(resp.content);
}

}  // namespace cortrix::memory
