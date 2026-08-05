#pragma once
#include <map>
#include <string>
#include <vector>

#include "cortrix/spc_enricher.h"  // ChunkContext

namespace cortrix::spc {

/// PromptTemplate. Builds the combined NER + Summary prompt
/// for a batch of chunks. V1 ships two templates: "default-zh" / "default-en".
///
/// The rendered prompt instructs the LLM to return a single JSON object keyed by
/// chunk index (topic 3.3 L1/L2/L3 fault-tolerant parsing on the response), with
/// per-chunk `entities` (text/type/offsets) + `summary` + `score`. Chunks are
/// delimited with `===CHUNK N===` markers (S2.3) so the instruction + the chunk
/// payload are unambiguous to the model.
class PromptTemplate {
public:
    /// @param template_id "default-zh" (fallback) / "default-en". Unknown ids
    ///                    fall back to "default-zh".
    explicit PromptTemplate(const std::string& template_id);

    /// Render the full prompt for `contexts` (batch). Each context contributes a
    /// `===CHUNK i===\n<chunk_text>` block; the leading instruction is the
    /// template body. Empty `contexts` → instruction only (no chunk blocks).
    std::string Render(const std::vector<ChunkContext>& contexts) const;

    /// The resolved template id actually in use (after the unknown→default-zh
    /// fallback) — surfaced into EnrichResult.prompt_version (S2.5).
    const std::string& id() const { return id_; }

    /// True iff `template_id` is a known built-in (no fallback applied).
    static bool IsKnown(const std::string& template_id);

private:
    std::string id_;
    const std::string* instruction_ = nullptr;  // points into kTemplates

    static const std::map<std::string, std::string>& Templates();
};

}  // namespace cortrix::spc
