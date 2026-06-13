#include "cortrix/spc_enricher/enricher_response_parser.h"

#include <nlohmann/json.hpp>

#include "cortrix/spc_enricher/enricher_error.h"

namespace cortrix::spc {

using json = nlohmann::json;

namespace {

// Fill a result as an L2/L3 parse failure (topic 1.6 / §5.1). score stays 0.
void FillParseError(EnrichResult& r, const char* layer, int batch_size,
                    const std::string& model) {
    r.status = static_cast<int>(EnricherErrorCode::kParse);
    r.enriched_score = 0.0f;
    json sd = {{"layer", layer}, {"batch_size", batch_size}};
    if (!model.empty()) sd["model"] = model;
    r.error_meta = EnricherErrorMeta::FromCode(EnricherErrorCode::kParse, sd.dump());
    r.error_msg = std::string(EnricherErrorCodeString(EnricherErrorCode::kParse)) +
                  " (" + layer + ")";
}

// L1: extract entities defensively — skip malformed array elements, default
// missing fields. Never throws (caller already has a parsed object).
std::vector<Entity> ParseEntities(const json& chunk_obj) {
    std::vector<Entity> out;
    auto it = chunk_obj.find("entities");
    if (it == chunk_obj.end() || !it->is_array()) return out;  // L1: missing → empty
    for (const json& e : *it) {
        if (!e.is_object()) continue;  // L1: skip malformed element
        Entity ent;
        ent.text = e.value("text", std::string{});
        ent.type = e.value("type", std::string{});
        ent.start_offset = e.value("start_offset", 0);
        ent.end_offset = e.value("end_offset", 0);
        out.push_back(std::move(ent));
    }
    return out;
}

}  // namespace

std::vector<EnrichResult> ParseEnrichBatchResponse(const std::string& body,
                                                   int batch_size,
                                                   const std::string& model,
                                                   const std::string& prompt_version) {
    std::vector<EnrichResult> results(static_cast<size_t>(batch_size < 0 ? 0 : batch_size));

    // --- L3: whole-batch parse ---
    json root = json::parse(body, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        for (auto& r : results) FillParseError(r, "L3", batch_size, model);
        return results;
    }

    for (int i = 0; i < batch_size; ++i) {
        EnrichResult& r = results[static_cast<size_t>(i)];
        // --- L2: chunk index key present? (accept "0" string key) ---
        auto it = root.find(std::to_string(i));
        if (it == root.end() || !it->is_object()) {
            FillParseError(r, "L2", batch_size, model);
            continue;
        }
        const json& chunk_obj = *it;

        // --- L1: per-field defaults; chunk stays success (status=0) ---
        r.status = 0;
        r.entities = ParseEntities(chunk_obj);
        r.summary = chunk_obj.value("summary", std::string{});  // L1: missing → ""
        // score: clamp into [0,1]; missing/malformed → 0 (L1).
        float score = 0.0f;
        if (auto sit = chunk_obj.find("score");
            sit != chunk_obj.end() && sit->is_number()) {
            score = sit->get<float>();
            if (score < 0.0f) score = 0.0f;
            if (score > 1.0f) score = 1.0f;
        }
        r.enriched_score = score;
        r.model_used = model;
        r.prompt_version = prompt_version;
        r.enricher_name = "LlmEnricher";
    }
    return results;
}

}  // namespace cortrix::spc
