#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/common/status.h"
#include "cortrix/spc/hype_enricher.h"   // HypeQuestion
#include "cortrix/spc/onnx_embedder.h"   // EmbeddingResult (FillHypeEmbedding)

namespace cortrix::spc {

/// Build the hype_question Block metadata_json for one question. Shape:
///   {"source_child_id","source_parent_id",
///    "hype":{"question_index","prompt_version","llm_provider","llm_model",
///            "cost_usd","generated_at"}}
/// `cost_usd` is the C2 soft-quota accounting input; `generated_at` is an ISO-8601
/// string. llm_provider defaults "openai" (Multi-LLM = Phase 2).
std::string BuildHypeQuestionMetadataJson(const HypeQuestion& q,
                                          const std::string& prompt_version,
                                          const std::string& llm_model,
                                          double cost_usd,
                                          const std::string& generated_at,
                                          const std::string& llm_provider = "openai");

/// Build a complete hype_question Block BLOB (128B header + payload) for one
/// question, via the frozen BlockBuild() with block_type=kBlockHypeQuestion (16).
/// content = question_text; metadata_json = BuildHypeQuestionMetadataJson(...);
/// vector_dim = q.embedding.size(). Proves a hype_question Block is a well-formed
/// Block that round-trips (block_type=16 is the recall discriminator — HyPE
/// owns no flags_ext bit, so flags_ext stays 0).
///
/// Standalone: the actual atomic write (chunk + hype_question Blocks in one
/// write-coordinator transaction) is wired separately; here we build
/// the BLOB the pipeline will hand to the write coordinator.
std::vector<uint8_t> BuildHypeQuestionBlock(const HypeQuestion& q,
                                            const std::string& prompt_version,
                                            const std::string& llm_model,
                                            double cost_usd,
                                            const std::string& generated_at);

/// Fill `q.embedding` from an embedder (the `hype_q.embedding =
/// OnnxEmbedder.Embed(question_text)` step). Templated on the embedder so it works
/// with the concrete OnnxEmbedder (stub mode) or a test FakeEmbedder — both expose
/// `Status Embed(const std::string&, EmbeddingResult*)`. Returns the embed Status.
///
/// Standalone: the real BGE-M3 inference is the OnnxEmbedder stub path here;
/// wiring the live pipeline embedder = integration.
template <typename Embedder>
Status FillHypeEmbedding(HypeQuestion& q, Embedder& embedder) {
    EmbeddingResult r;
    Status s = embedder.Embed(q.question_text, &r);
    if (!s.ok()) return s;
    q.embedding = std::move(r.vector);
    return Status::Ok();
}

}  // namespace cortrix::spc
