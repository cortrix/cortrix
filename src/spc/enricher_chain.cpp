#include "cortrix/spc/enricher_chain.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "cortrix/logging/logging.h"

namespace cortrix::spc {

namespace {

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Known enricher tokens (GS-2). Unknown tokens are dropped (fail-soft).
bool IsKnownToken(const std::string& t) {
    return t == "enrich" || t == "contextual" || t == "hype";
}

}  // namespace

std::vector<std::string> ParseEnricherChainSpec(const std::string& spec) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::stringstream ss(spec);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        std::string t = Lower(Trim(tok));
        if (t.empty() || !IsKnownToken(t)) continue;
        if (seen.insert(t).second) out.push_back(t);
    }
    // The `enrich` enricher always leads. If any token is present but enrich is not, prepend it
    // (`contextual`/`hype` are chain peers AFTER `enrich`). An empty/all-unknown spec defaults to
    // the single-enricher {"enrich"} chain (L1).
    if (out.empty()) return {"enrich"};
    if (std::find(out.begin(), out.end(), "enrich") == out.end()) {
        out.insert(out.begin(), "enrich");
    } else if (out.front() != "enrich") {
        out.erase(std::remove(out.begin(), out.end(), "enrich"), out.end());
        out.insert(out.begin(), "enrich");
    }
    return out;
}

std::vector<std::string> ResolveEnricherChain(const IGlobalConfig* global,
                                              const std::string& ns_metadata_json) {
    // NS metadata override wins (the per-NS "enricher_chain" string, the NS
    // layer pattern). Falls back to the global `enricher.chain` GUC, then the
    // built-in default.
    if (!ns_metadata_json.empty()) {
        auto j = nlohmann::json::parse(ns_metadata_json, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded() && j.is_object()) {
            auto it = j.find("enricher_chain");
            if (it != j.end() && it->is_string()) {
                return ParseEnricherChainSpec(it->get<std::string>());
            }
        }
    }
    if (global) {
        auto r = global->GetString("enricher.chain");
        if (r.ok() && !r.value().empty()) {
            return ParseEnricherChainSpec(r.value());
        }
    }
    return {"enrich"};
}

void EnricherChain::Append(std::shared_ptr<ISpcEnricher> enricher) {
    if (enricher) enrichers_.push_back(std::move(enricher));
}

bool EnricherChain::AnyAvailable() const {
    for (const auto& e : enrichers_) {
        if (e && e->IsAvailable()) return true;
    }
    return false;
}

std::vector<std::string> EnricherChain::Names() const {
    std::vector<std::string> names;
    names.reserve(enrichers_.size());
    for (const auto& e : enrichers_) {
        if (e) names.push_back(e->Name());
    }
    return names;
}

std::string ChainMemberToken(const std::string& enricher_name) {
    if (enricher_name == "hype") return "hype";
    if (enricher_name == "contextual_retrieval") return "contextual";
    return "enrich";  // the head slot (LlmEnricher / LocalNer / test fakes)
}

std::vector<ChunkChainResult> EnricherChain::EnrichChunks(
    const std::vector<ChunkContext>& contexts,
    const std::vector<std::string>& parent_texts,
    const std::vector<std::string>& source_child_ids,
    const std::vector<std::string>& source_parent_ids,
    const std::unordered_set<std::string>* member_filter) {
    std::vector<ChunkChainResult> results(contexts.size());

    for (const auto& enricher : enrichers_) {
        if (!enricher || !enricher->IsAvailable()) {
            // Soft-skip: record the skip on every chunk so observability sees it.
            if (enricher) {
                for (auto& r : results) {
                    r.steps.push_back({enricher->Name(), 0, "", /*skipped=*/true});
                }
            }
            continue;
        }
        const std::string name = enricher->Name();
        // addendum backfill: only the owed members run — a filtered-out
        // member records a skipped step (same bookkeeping as unavailable), so the
        // debt detection downstream never re-owes an already-repaired member.
        if (member_filter &&
            member_filter->find(ChainMemberToken(name)) == member_filter->end()) {
            for (auto& r : results) r.steps.push_back({name, 0, "", /*skipped=*/true});
            continue;
        }

        // The hype enricher has a side channel (GenerateHypeQuestions): the frozen EnrichResult
        // carries no hype slot (reconcile 1). Detect it by Name() == "hype" and
        // route to its dedicated API; everything else flows through EnrichBatch and
        // the enricher and contextual EnrichResult fields are merged.
        if (name == "hype") {
            auto* hype = dynamic_cast<HyPEEnricher*>(enricher.get());
            if (hype) {
                for (size_t i = 0; i < contexts.size(); ++i) {
                    const std::string parent_text =
                        i < parent_texts.size() ? parent_texts[i] : "";
                    const std::string child_id =
                        i < source_child_ids.size() ? source_child_ids[i] : "";
                    const std::string parent_id =
                        i < source_parent_ids.size() ? source_parent_ids[i] : "";
                    const DocumentMetadata* dm = contexts[i].doc_metadata;
                    DocumentMetadata empty_dm;
                    auto qres = hype->GenerateHypeQuestions(
                        contexts[i].chunk_text, parent_text, dm ? *dm : empty_dm,
                        child_id, parent_id);
                    if (qres.ok()) {
                        auto& qs = qres.value();
                        results[i].hype_questions.insert(results[i].hype_questions.end(),
                                                         qs.begin(), qs.end());
                        results[i].steps.push_back({name, 0, "", false});
                    } else {
                        // Fail-soft: HyPE degraded — record the CX_ERR_HYPE_* token,
                        // skip this chunk's questions, continue.
                        results[i].steps.push_back(
                            {name, 1, qres.status().message(), false});
                        CORTRIX_LOG_DEBUG(
                            "spc", "hype degraded for chunk {}: {}", i,
                            qres.status().message());
                    }
                }
            }
            continue;
        }

        // Enricher / contextual: run EnrichBatch + merge each result into the chunk's merged
        // EnrichResult. Wrapped so a throwing enricher is caught (fail-soft) and the
        // chain continues with the partial result already accumulated.
        std::vector<EnrichResult> batch;
        try {
            batch = enricher->EnrichBatch(contexts);
        } catch (const std::exception& ex) {
            for (auto& r : results) r.steps.push_back({name, 1, ex.what(), false});
            CORTRIX_LOG_WARN("spc", "enricher {} threw, skipped: {}", name, ex.what());
            continue;
        } catch (...) {
            for (auto& r : results) r.steps.push_back({name, 1, "unknown", false});
            CORTRIX_LOG_WARN("spc", "enricher {} threw (unknown), skipped", name);
            continue;
        }

        for (size_t i = 0; i < batch.size() && i < results.size(); ++i) {
            EnrichResult& step = batch[i];
            EnrichResult& merged = results[i].merged;
            const bool step_ok = step.ok();

            // Merge policy: the enricher head establishes the base (entities/summary/
            // enriched_score/provenance). The contextual stage contributes the contextualized_* fields
            // only. We never clobber a non-empty enricher field with an empty later one.
            if (!step.entities.empty()) merged.entities = std::move(step.entities);
            if (!step.summary.empty()) merged.summary = std::move(step.summary);
            if (step.enriched_score > 0.0f) merged.enriched_score = step.enriched_score;
            if (!step.enricher_name.empty() && merged.enricher_name.empty())
                merged.enricher_name = step.enricher_name;
            if (step.token_count > 0) merged.token_count += step.token_count;
            if (step.duration_ms > 0) merged.duration_ms += step.duration_ms;
            if (!step.model_used.empty()) merged.model_used = step.model_used;
            if (!step.prompt_version.empty()) merged.prompt_version = step.prompt_version;

            // contextualized_* (pure-ADD fields; only the contextual stage populates them).
            if (step.contextualized_text.has_value())
                merged.contextualized_text = std::move(step.contextualized_text);
            if (step.contextualized_embedding.has_value())
                merged.contextualized_embedding = std::move(step.contextualized_embedding);
            if (step.contextualized_status != 0)
                merged.contextualized_status = step.contextualized_status;

            std::string err_code;
            if (!step_ok) {
                err_code = step.error_meta.structured_data.empty()
                               ? step.error_msg
                               : step.error_meta.structured_data;
                // Surface the first hard error onto the merged status so the write
                // phase can decide; later successful steps do not clear it.
                if (merged.status == 0) {
                    merged.status = step.status;
                    merged.error_meta = step.error_meta;
                    merged.error_msg = step.error_msg;
                }
            } else if (step.contextualized_status == 2 && !step.error_msg.empty()) {
                // Contextual fail-soft: the step reports OK (the chunk keeps its original
                // embedding) but the member outcome is a failure recorded only in
                // contextualized_status. Without this, the contextual debt row is written
                // with an empty last_error and the real cause (e.g. the
                // injection-guard byte limit) is unobservable — 2026-07-11 live 5k
                // ingest: 4,139 such rows, all blank.
                err_code = step.error_msg;
            }
            results[i].steps.push_back({name, step.status, err_code, false});
        }
    }

    return results;
}

}  // namespace cortrix::spc
