#include <cstdint>
#include "cortrix/spc/hype_block.h"

#include <nlohmann/json.hpp>

namespace cortrix::spc {

std::string BuildHypeQuestionMetadataJson(const HypeQuestion& q,
                                          const std::string& prompt_version,
                                          const std::string& llm_model,
                                          double cost_usd,
                                          const std::string& generated_at,
                                          const std::string& llm_provider) {
    // metadata_json layout.
    nlohmann::json j;
    j["source_child_id"] = q.source_child_id;
    j["source_parent_id"] = q.source_parent_id;
    j["hype"] = {
        {"question_index", q.question_index},
        {"prompt_version", prompt_version},
        {"llm_provider", llm_provider},
        {"llm_model", llm_model},
        {"cost_usd", cost_usd},
        {"generated_at", generated_at},
    };
    return j.dump();
}

std::vector<uint8_t> BuildHypeQuestionBlock(const HypeQuestion& q,
                                            const std::string& prompt_version,
                                            const std::string& llm_model,
                                            double cost_usd,
                                            const std::string& generated_at) {
    std::string metadata_json = BuildHypeQuestionMetadataJson(
        q, prompt_version, llm_model, cost_usd, generated_at);
    // block_type=16; content = question text; vector_dim from the embedding.
    // flags_ext = 0 (HyPE owns no flags_ext bit; block_type=16 is the
    // discriminator). processing_chain empty; enrichment_source = LLM (the
    // question was LLM-generated). Level L3 (vector + BM25 + metadata).
    return BlockBuild(
        /*block_type=*/kBlockHypeQuestion,
        /*level=*/kLevelL3,
        /*content_text=*/q.question_text,
        /*metadata_json=*/metadata_json,
        /*processing_chain=*/"",
        /*vector_dim=*/static_cast<uint16_t>(q.embedding.size()),
        /*embedding_model_id=*/0,
        /*flags_ext=*/0,
        /*enrichment_source=*/static_cast<uint8_t>(kEnrichLlm));
}

}  // namespace cortrix::spc
