#include "cortrix/spc_enricher/enricher_response_parser.h"

#include <cerrno>
#include <cstdlib>
#include <limits>

#include <nlohmann/json.hpp>

#include "cortrix/common/json_contract.h"
#include "cortrix/logging/logging.h"
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

int ReadIntField(const json& obj, const char* key, int fallback = 0) {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_number_unsigned()) {
        const auto v = it->get<unsigned long long>();
        if (v > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
            return fallback;
        }
        return static_cast<int>(v);
    }
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        char* end = nullptr;
        errno = 0;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (errno == 0 && end != s.c_str() && *end == '\0' &&
            v >= std::numeric_limits<int>::min() &&
            v <= std::numeric_limits<int>::max()) {
            return static_cast<int>(v);
        }
    }
    return fallback;
}

// L1: extract entities defensively — skip malformed array elements, default
// missing OR wrong-typed fields. Never throws (caller already has a parsed
// object). NOTE: json::value(key, default) only covers the missing case — if the
// key is present with the wrong type (e.g. a reasoning LLM emits
// "start_offset":"5" as a string), value<int>() internally calls get<int>() and
// throws type_error.302. That escaped here and dropped the whole batch's
// enrichment, defeating the per-field-default L1 contract. Guard numeric fields
// with is_number() (mirrors the score handling in ParseEnrichBatchResponse).
std::vector<Entity> ParseEntities(const json& chunk_obj) {
    std::vector<Entity> out;
    auto it = chunk_obj.find("entities");
    if (it == chunk_obj.end() || !it->is_array()) return out;  // L1: missing → empty
    for (const json& e : *it) {
        if (!e.is_object()) continue;  // L1: skip malformed element
        Entity ent;
        ent.text = e.value("text", std::string{});
        ent.type = e.value("type", std::string{});
        ent.start_offset = ReadIntField(e, "start_offset");
        ent.end_offset = ReadIntField(e, "end_offset");
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
    const std::string normalized_body = common::UnwrapCompleteJsonFence(body);
    json root = json::parse(normalized_body, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        // Second chance (QA 2026-07-12): the doc-summary contract already repairs
        // invalid provider backslash escapes; without the same repair here one
        // bad chunk string poisons the WHOLE batch (L3 is all-or-nothing) and
        // retries fail deterministically on the same text.
        const std::string repaired =
            common::EscapeInvalidJsonStringBackslashes(normalized_body);
        if (repaired != normalized_body) {
            root = json::parse(repaired, /*cb=*/nullptr, /*allow_exceptions=*/false);
            if (!root.is_discarded() && root.is_object()) {
                // Observable-by-design (json_contract.h contract, QA F-14): a
                // repair marks a provider-contract deviation — surface it, never
                // let it become the silent normal path.
                CORTRIX_LOG_WARN("spc",
                    "F03 batch response accepted via escape repair (invalid "
                    "backslash escape from provider; batch_size={} model={})",
                    batch_size, model);
            }
        }
    }
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
