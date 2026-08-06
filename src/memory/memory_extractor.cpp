#include <cstdint>
#include "cortrix/memory/memory_extractor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "cortrix/id/ulid.h"
#include "cortrix/memory/contradiction_detector.h"
#include "cortrix/memory/memory_extract_metrics.h"
#include "cortrix/memory/prompt_templates.h"

namespace cortrix::memory {

namespace {

/// ISO 8601 UTC "YYYY-MM-DDTHH:MM:SSZ" for `now`.
std::string NowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

/// Parse an ISO 8601 "YYYY-MM-DDTHH:MM:SSZ" timestamp to Unix seconds. Returns -1 on
/// a malformed string (caller treats -1 as "unknown" → outside any window).
long long ParseIso8601ToEpoch(const std::string& iso) {
    if (iso.size() < 19) return -1;
    std::tm tm_utc{};
    int y, mo, d, h, mi, s;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
        return -1;
    }
    tm_utc.tm_year = y - 1900;
    tm_utc.tm_mon = mo - 1;
    tm_utc.tm_mday = d;
    tm_utc.tm_hour = h;
    tm_utc.tm_min = mi;
    tm_utc.tm_sec = s;
#if defined(_WIN32)
    return static_cast<long long>(_mkgmtime(&tm_utc));
#else
    return static_cast<long long>(timegm(&tm_utc));
#endif
}

}  // namespace

const char* ToString(MemoryType type) {
    switch (type) {
        case MemoryType::kFact:       return "fact";
        case MemoryType::kPreference: return "preference";
        case MemoryType::kEvent:      return "event";
        case MemoryType::kUnknown:    return "unknown";
    }
    return "unknown";
}

MemoryType ParseMemoryType(const std::string& s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "fact") return MemoryType::kFact;
    if (lower == "preference") return MemoryType::kPreference;
    if (lower == "event") return MemoryType::kEvent;
    return MemoryType::kUnknown;  // D4: caller coerces to event
}

const char* ToString(MemoryStatus status) {
    switch (status) {
        case MemoryStatus::kActive:      return "active";
        case MemoryStatus::kTentative:   return "tentative";
        case MemoryStatus::kInvalidated: return "invalidated";
    }
    return "active";
}

MemoryExtractor::MemoryExtractor(std::shared_ptr<llm::ILlmClient> llm,
                                 std::shared_ptr<IMemoryBlockStore> block_store,
                                 std::shared_ptr<IContradictionQuery> contradiction_query,
                                 std::shared_ptr<observability::IOperationLogger> op_logger,
                                 MemoryExtractorConfig config)
    : llm_(std::move(llm)),
      block_store_(std::move(block_store)),
      contradiction_query_(std::move(contradiction_query)),
      op_logger_(std::move(op_logger)),
      config_(std::move(config)) {}

std::string MemoryExtractor::BuildWindowText(const std::vector<InteractionLog>& window) const {
    // Keep only the most recent `window_size_default` turns (D3). The window is
    // oldest→newest; we take the tail.
    const int max_turns = std::max(1, config_.window_size_default);
    size_t start = 0;
    if (window.size() > static_cast<size_t>(max_turns)) {
        start = window.size() - static_cast<size_t>(max_turns);
    }
    std::string out;
    for (size_t i = start; i < window.size(); ++i) {
        const auto& turn = window[i];
        if (!turn.role.empty()) {
            out += turn.role;
            out += ": ";
        }
        out += turn.content;
        out += "\n";
    }
    return out;
}

Result<std::vector<ExtractedMemory>>
MemoryExtractor::ParseExtractionJson(const std::string& llm_output) const {
    nlohmann::json j = nlohmann::json::parse(llm_output, /*cb=*/nullptr,
                                             /*allow_exceptions=*/false);
    // LLMs frequently wrap the JSON in a ```json … ``` markdown fence, add a line of
    // prose, or nest the array inside an object — even with response_format=json_object
    // (GLM, Claude and GPT all do it intermittently; raw json::parse then fails as
    // "not a JSON array"). Fallback: narrow to the outermost [ … ] span and re-parse.
    // This tolerates fences, surrounding prose, and {"...":[ … ]} object wrappers.
    if (j.is_discarded() || !j.is_array()) {
        const auto lb = llm_output.find('[');
        const auto rb = llm_output.rfind(']');
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            j = nlohmann::json::parse(llm_output.substr(lb, rb - lb + 1), /*cb=*/nullptr,
                                      /*allow_exceptions=*/false);
        }
    }
    if (j.is_discarded() || !j.is_array()) {
        return MemoryExtractStatus(MemoryExtractErrorCode::kExtractInvalidOutput,
                           "extraction output is not a JSON array");
    }
    std::vector<ExtractedMemory> out;
    out.reserve(j.size());
    for (const auto& item : j) {
        if (!item.is_object() || !item.contains("content") ||
            !item["content"].is_string()) {
            // Skip malformed entries rather than failing the whole batch — but an
            // entry with no usable content is a contract violation worth flagging.
            return MemoryExtractStatus(MemoryExtractErrorCode::kExtractInvalidOutput,
                               "extraction entry missing string 'content'");
        }
        ExtractedMemory mem;
        mem.content = item["content"].get<std::string>();
        // D4 fallback: unknown/missing type → event.
        MemoryType t = MemoryType::kUnknown;
        if (item.contains("type") && item["type"].is_string()) {
            t = ParseMemoryType(item["type"].get<std::string>());
        }
        mem.type = (t == MemoryType::kUnknown) ? MemoryType::kEvent : t;
        if (item.contains("confidence") && item["confidence"].is_number()) {
            mem.confidence = item["confidence"].get<double>();
            if (mem.confidence < 0.0) mem.confidence = 0.0;
            if (mem.confidence > 1.0) mem.confidence = 1.0;
        }
        mem.extraction_method = "llm";
        out.push_back(std::move(mem));
    }
    return out;
}

Result<MemoryExtractor::Judgment>
MemoryExtractor::ParseJudgmentJson(const std::string& llm_output) const {
    return ContradictionDetector::ParseJudgmentJson(llm_output);
}

MemoryExtractionResult MemoryExtractor::DisabledResult(const std::string& interaction_id) const {
    MemoryExtractionResult r;
    nlohmann::json sd = {{"reason", "memory_extract.enabled=false (NullEnricher mode)"}};
    if (!interaction_id.empty()) sd["interaction_id"] = interaction_id;
    r.error = MakeMemoryExtractError(MemoryExtractErrorCode::kLlmDisabled, sd,
                             "LLM memory extraction is disabled for this namespace");
    return r;
}

MemoryExtractionResult MemoryExtractor::Extract(const InteractionLog& current_turn,
                                                const observability::TraceContext* ctx) {
    // Standalone: the real multi-turn get_window pulls preceding turns from the store
    // (D3.5 wiring). Here we extract over the single current turn as the window.
    return ExtractFromWindow({current_turn}, ctx);
}

MemoryExtractionResult MemoryExtractor::ExtractFromWindow(
    const std::vector<InteractionLog>& window,
    const observability::TraceContext* ctx) {
    MemoryExtractionResult result;

    const std::string interaction_id = window.empty() ? std::string() : window.back().id;
    const std::string ns = window.empty() ? std::string() : window.back().namespace_name;
    const std::string user_id = window.empty() ? std::string() : window.back().user_id;
    const std::string session_id = window.empty() ? std::string() : window.back().session_id;

    auto& metrics = MemoryExtractMetrics::Instance();

    if (!config_.enabled || !llm_) {
        metrics.RecordExtract(MemoryExtractMetrics::ExtractStatus::kFailed);
        return DisabledResult(interaction_id);
    }
    if (window.empty()) {
        metrics.RecordExtract(MemoryExtractMetrics::ExtractStatus::kFailed);
        result.error = MakeMemoryExtractError(MemoryExtractErrorCode::kExtractInvalidOutput,
                                      {{"interaction_id", ""}, {"llm_model", config_.llm_model}},
                                      "empty interaction window");
        return result;
    }

    // 1. Build prompt + call LLM (D3 / D10).
    const std::string window_text = BuildWindowText(window);
    const char* tmpl = ExtractPromptTemplate(PromptLang::kAuto, window_text);
    const std::string prompt = RenderPrompt(tmpl, kPlaceholderInteractionWindow, window_text);

    llm::LlmCallConfig cfg;
    cfg.model = config_.llm_model;
    cfg.timeout_ms = config_.llm_timeout_ms;
    cfg.response_format = "json_object";

    const auto t0 = std::chrono::steady_clock::now();
    llm::ChatCompletionResponse resp = llm_->Chat(prompt, cfg);
    const auto t1 = std::chrono::steady_clock::now();
    const int latency_ms =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    const std::string used_model = resp.model.empty() ? config_.llm_model : resp.model;
    metrics.ObserveExtractDuration(used_model, latency_ms);
    metrics.RecordTokens(used_model, MemoryExtractMetrics::TokenDirection::kInput,
                         static_cast<uint64_t>(std::max(0, resp.prompt_tokens)));
    metrics.RecordTokens(used_model, MemoryExtractMetrics::TokenDirection::kOutput,
                         static_cast<uint64_t>(std::max(0, resp.completion_tokens)));

    if (!resp.ok()) {
        metrics.RecordExtract(MemoryExtractMetrics::ExtractStatus::kFailed);
        // A client-reported kUnavailable is the timeout/circuit family. Other
        // failures surface as INVALID_OUTPUT (the model produced nothing usable).
        if (resp.status.code() == StatusCode::kUnavailable) {
            result.error = MakeMemoryExtractError(
                MemoryExtractErrorCode::kExtractLlmTimeout,
                {{"interaction_id", interaction_id},
                 {"llm_model", config_.llm_model},
                 {"timeout_ms", config_.llm_timeout_ms}},
                "LLM extraction timed out for interaction " + interaction_id);
        } else if (resp.status.code() == StatusCode::kPermissionDenied) {
            result.error = MakeMemoryExtractError(
                MemoryExtractErrorCode::kExtractBudgetExceeded,
                {{"budget_cap_usd", 0}, {"current_usage_usd", 0}},
                "LLM budget exceeded: " + resp.status.message());
        } else {
            result.error = MakeMemoryExtractError(
                MemoryExtractErrorCode::kExtractInvalidOutput,
                {{"interaction_id", interaction_id}, {"llm_model", config_.llm_model}},
                "LLM extraction failed: " + resp.status.message());
        }
        return result;
    }

    // 2. Parse extraction JSON (D4 fallback applied inside).
    Result<std::vector<ExtractedMemory>> parsed = ParseExtractionJson(resp.content);
    if (!parsed.ok()) {
        metrics.RecordExtract(MemoryExtractMetrics::ExtractStatus::kFailed);
        result.error = MakeMemoryExtractError(
            MemoryExtractErrorCode::kExtractInvalidOutput,
            {{"interaction_id", interaction_id}, {"llm_model", config_.llm_model}},
            parsed.status().message());
        return result;
    }
    std::vector<ExtractedMemory> memories = std::move(parsed.value());

    // Stamp provenance (T-109/T-110).
    for (auto& m : memories) {
        m.source_interaction_id = interaction_id;
        m.source_session_id = session_id;
    }

    // 3. Contradiction judgment (D5) over the extracted facts.
    std::vector<ContradictionPair> contradictions =
        FindContradictions(ns, memories, ctx);

    // 4. Persist + stamp invalidation + operation_log (D6 / §4.2.3).
    Status wstatus = WriteWithOperationLog(ns, user_id, memories, contradictions, ctx);
    if (!wstatus.ok()) {
        metrics.RecordExtract(MemoryExtractMetrics::ExtractStatus::kFailed);
        result.error = MakeMemoryExtractError(
            MemoryExtractErrorCode::kExtractInvalidOutput,
            {{"interaction_id", interaction_id}, {"llm_model", config_.llm_model}},
            "persist failed: " + wstatus.message());
        return result;
    }

    // 5. Assemble result + extract_meta (D11).
    ExtractMeta meta;
    meta.llm_model = used_model;
    meta.llm_latency_ms = latency_ms;
    meta.tokens_used = std::max(0, resp.prompt_tokens) + std::max(0, resp.completion_tokens);
    meta.tokens_cost_usd =
        (config_.tokens_cost_per_1k_usd > 0.0)
            ? (static_cast<double>(meta.tokens_used) / 1000.0) * config_.tokens_cost_per_1k_usd
            : 0.0;
    meta.interaction_window_size = std::min<int>(
        static_cast<int>(window.size()), std::max(1, config_.window_size_default));
    meta.contradictions_found = static_cast<int>(contradictions.size());
    meta.extracted_at = NowIso8601();

    result.extracted_memories = std::move(memories);
    result.extract_meta = std::move(meta);
    metrics.RecordExtract(MemoryExtractMetrics::ExtractStatus::kSuccess);
    return result;
}

MemoryExtractionBatchResult MemoryExtractor::ExtractBatch(
    const std::vector<InteractionLog>& turns,
    const observability::TraceContext* ctx) {
    MemoryExtractionBatchResult batch;
    batch.results.reserve(turns.size());
    for (const auto& turn : turns) {
        batch.results.push_back(Extract(turn, ctx));
    }
    return batch;
}

std::vector<ContradictionPair> MemoryExtractor::FindContradictions(
    const std::string& ns,
    const std::vector<ExtractedMemory>& new_memories,
    const observability::TraceContext* ctx) {
    std::vector<ContradictionPair> pairs;
    if (!contradiction_query_ || !llm_) return pairs;

    ContradictionDetector judge(llm_, config_.llm_model, config_.llm_timeout_ms);
    auto& metrics = MemoryExtractMetrics::Instance();

    for (const auto& nm : new_memories) {
        // Only facts participate in contradiction (D5: "judge whether the new fact contradicts the old fact" —
        // the judge is fact-centric; preferences upgrade via the D8 mention-count
        // path, and events are temporal — "temporary event vs long-term fact" is
        // explicitly NOT a contradiction).
        if (nm.type != MemoryType::kFact) continue;

        std::vector<CandidateBlock> candidates =
            contradiction_query_->FindCandidates(ns, nm.content, config_.contradiction_top_k);
        for (const auto& cand : candidates) {
            // Judge only against existing facts (same fact-centric rationale).
            if (cand.type != MemoryType::kFact) continue;
            Result<Judgment> jr = judge.Judge(nm.content, cand.content, ctx);
            if (!jr.ok()) {
                // Ambiguous / timed-out judge: skip this candidate (do not invalidate
                // on an unreliable verdict — R2 mitigation). The fault is surfaced via
                // metrics; the extract path itself is not failed by one bad judge call.
                continue;
            }
            const Judgment& jm = jr.value();
            if (jm.is_contradiction) {
                ContradictionPair p;
                p.new_content = nm.content;
                p.old_block_id = cand.block_id;
                p.old_content = cand.content;
                p.reason = jm.reason;
                p.confidence = jm.confidence;
                pairs.push_back(std::move(p));
                metrics.RecordContradiction(
                    MemoryExtractMetrics::BucketForConfidence(jm.confidence));
            }
        }
    }
    return pairs;
}

bool MemoryExtractor::MaybeUpgradePreference(const std::string& ns,
                                             const std::string& now_iso,
                                             const ExtractedMemory& mem,
                                             const observability::TraceContext* ctx) {
    if (!contradiction_query_ || !block_store_) return false;

    std::optional<CandidateBlock> match =
        contradiction_query_->FindMatchingPreference(ns, mem.content);
    if (!match.has_value()) return false;

    Result<MemoryBlockRecord> br = block_store_->GetMemoryBlock(match->block_id);
    if (!br.ok()) return false;  // existing block gone; caller inserts fresh
    MemoryBlockRecord block = br.value();
    if (!block.metadata_json.is_object()) {
        block.metadata_json = nlohmann::json::object();
    }

    // Increment mention_count (D8). Read the existing count defensively.
    int count = 1;
    if (block.metadata_json.contains("mention_count") &&
        block.metadata_json["mention_count"].is_number_integer()) {
        count = block.metadata_json["mention_count"].get<int>();
    }
    std::string first_at = now_iso;
    if (block.metadata_json.contains("first_mentioned_at") &&
        block.metadata_json["first_mentioned_at"].is_string()) {
        first_at = block.metadata_json["first_mentioned_at"].get<std::string>();
    }

    block.metadata_json["mention_count"] = count + 1;
    // D8 immunity: once count+1 >= threshold within the window, the preference is
    // immune (status stays active — the scorer reads status=active → weight ×1.0; the
    // immune flag records that it no longer decays as an event-like preference).
    const bool immune = IsPreferenceImmune(
        count, first_at, now_iso,
        config_.preference_immunization_threshold,
        config_.preference_immunization_window_days);
    block.metadata_json["immune"] = immune;
    block.metadata_json["status"] = ToString(MemoryStatus::kActive);

    Status us = block_store_->UpdateMemoryBlock(block);
    if (!us.ok()) return false;

    if (op_logger_) {
        observability::OperationLogEntry e;
        e.user_id = block.user_id.empty() ? "anonymous" : block.user_id;
        e.action = "memory_blocks_update";
        e.resource_type = "memory";
        e.resource_id = block.block_id;
        if (!ns.empty()) e.namespace_id = ns;
        char summary[128];
        std::snprintf(summary, sizeof(summary),
                      "[auto] Preference Block_%s mention_count -> %d%s",
                      block.block_id.c_str(), count + 1,
                      immune ? " (immune)" : "");
        e.summary = std::string(summary);
        op_logger_->Log(e, ctx);
    }
    return true;
}

Status MemoryExtractor::WriteWithOperationLog(
    const std::string& ns,
    const std::string& user_id,
    std::vector<ExtractedMemory>& new_memories,
    const std::vector<ContradictionPair>& contradictions,
    const observability::TraceContext* ctx) {
    if (!block_store_) {
        return Status::Internal("CX_ERR_MEMEXTRACT_INVALID_OUTPUT: no block store");
    }
    auto& metrics = MemoryExtractMetrics::Instance();
    const std::string now = NowIso8601();

    // 1. Insert each new memory block with the §4.1 metadata_json fields; mint a ULID.
    //    A repeat preference (D8) upgrades the existing block's mention_count instead
    //    of inserting a duplicate.
    for (auto& m : new_memories) {
        if (m.type == MemoryType::kPreference &&
            MaybeUpgradePreference(ns, now, m, ctx)) {
            continue;  // existing preference upgraded; no new block
        }
        m.block_id = id::GenerateUlid();
        MemoryBlockRecord rec;
        rec.block_id = m.block_id;
        rec.ns_id = ns;
        rec.user_id = user_id;
        rec.content = m.content;
        rec.metadata_json = {
            {"memory_type", ToString(m.type)},
            {"status", ToString(MemoryStatus::kActive)},
            {"created_at", now},
            {"source_session_id", m.source_session_id},
            {"source_interaction_id", m.source_interaction_id},
            {"extraction_method", m.extraction_method},
            {"confidence", m.confidence},
            {"llm_model", config_.llm_model},
            {"user_id", user_id},
        };
        if (m.type == MemoryType::kPreference) {
            // First mention (D8): count starts at 1, not yet immune.
            rec.metadata_json["mention_count"] = 1;
            rec.metadata_json["first_mentioned_at"] = now;
        }
        Result<std::string> ins = block_store_->InsertMemoryBlock(rec);
        if (!ins.ok()) {
            return ins.status();
        }
    }

    // 2. Stamp invalidation state on contradicted old blocks (D6 / §4.2.3) + record
    //    which new block invalidated each.
    for (const auto& c : contradictions) {
        // Find the minting new block for this contradiction (match by content).
        std::string new_block_id;
        for (auto& m : new_memories) {
            if (m.content == c.new_content) {
                new_block_id = m.block_id;
                m.invalidates.push_back(c.old_block_id);
                break;
            }
        }

        Result<MemoryBlockRecord> oldr = block_store_->GetMemoryBlock(c.old_block_id);
        if (!oldr.ok()) {
            // Old block vanished (race) — skip; the new memory is already persisted.
            continue;
        }
        MemoryBlockRecord old_block = oldr.value();
        if (!old_block.metadata_json.is_object()) {
            old_block.metadata_json = nlohmann::json::object();
        }
        old_block.metadata_json["status"] = ToString(MemoryStatus::kInvalidated);
        old_block.metadata_json["invalidated_by_block_id"] = new_block_id;
        old_block.metadata_json["invalidated_at"] = now;
        old_block.metadata_json["invalidation_reason"] = c.reason;
        old_block.metadata_json["invalidation_confidence"] = c.confidence;
        old_block.metadata_json["invalidation_triggered_by"] = "llm_auto";
        old_block.metadata_json["auto_revoke_eligible"] =
            (c.confidence < config_.auto_revoke_confidence_threshold);
        Status us = block_store_->UpdateMemoryBlock(old_block);
        if (!us.ok()) {
            return us;
        }
        metrics.RecordInvalidation(MemoryExtractMetrics::TriggeredBy::kLlmAuto);

        // operation_log: memory_invalidate (§4.2.2 — action {resource}_{verb}).
        if (op_logger_) {
            observability::OperationLogEntry e;
            e.user_id = user_id.empty() ? "anonymous" : user_id;
            e.action = "memory_invalidate";
            e.resource_type = "memory";
            e.resource_id = c.old_block_id;
            if (!ns.empty()) e.namespace_id = ns;
            char summary[128];
            std::snprintf(summary, sizeof(summary),
                          "[auto] Block_%s invalidated by Block_%s (confidence: %.2f)",
                          c.old_block_id.c_str(), new_block_id.c_str(), c.confidence);
            e.summary = std::string(summary);
            op_logger_->Log(e, ctx);
        }
    }

    // 3. operation_log: memory_extract (one summary row for the batch, §4.2.2).
    if (op_logger_ && !new_memories.empty()) {
        observability::OperationLogEntry e;
        e.user_id = user_id.empty() ? "anonymous" : user_id;
        e.action = "memory_extract";
        e.resource_type = "memory";
        if (!new_memories.front().source_interaction_id.empty()) {
            e.resource_id = new_memories.front().source_interaction_id;
        }
        if (!ns.empty()) e.namespace_id = ns;
        char summary[128];
        std::snprintf(summary, sizeof(summary),
                      "[auto] Extracted %zu memories from interaction %s (model: %s)",
                      new_memories.size(),
                      new_memories.front().source_interaction_id.c_str(),
                      config_.llm_model.c_str());
        e.summary = std::string(summary);
        op_logger_->Log(e, ctx);
    }

    return Status::Ok();
}

Result<MemoryBlockRecord> MemoryExtractor::RevokeInvalidation(
    const std::string& invalidated_block_id,
    const std::string& reason,
    const std::string& revoked_by,
    const observability::TraceContext* ctx) {
    if (!block_store_) {
        return Status::Internal("CX_ERR_MEMEXTRACT_INVALID_OUTPUT: no block store");
    }
    Result<MemoryBlockRecord> br = block_store_->GetMemoryBlock(invalidated_block_id);
    if (!br.ok()) {
        return br.status();
    }
    MemoryBlockRecord block = br.value();
    if (!block.metadata_json.is_object()) {
        block.metadata_json = nlohmann::json::object();
    }
    const std::string now = NowIso8601();
    // Restore: status invalidated → active; record the revocation provenance.
    block.metadata_json["status"] = ToString(MemoryStatus::kActive);
    block.metadata_json["revoked_at"] = now;
    block.metadata_json["revoked_by"] = revoked_by;
    block.metadata_json["revoke_reason"] = reason;
    Status us = block_store_->UpdateMemoryBlock(block);
    if (!us.ok()) {
        return us;
    }
    MemoryExtractMetrics::Instance().RecordInvalidation(
        revoked_by == "agent_self" ? MemoryExtractMetrics::TriggeredBy::kAgentSelf
                                   : MemoryExtractMetrics::TriggeredBy::kManual);

    if (op_logger_) {
        observability::OperationLogEntry e;
        e.user_id = revoked_by.empty() ? "anonymous" : revoked_by;
        e.action = "memory_revoke";
        e.resource_type = "memory";
        e.resource_id = invalidated_block_id;
        if (!block.ns_id.empty()) e.namespace_id = block.ns_id;
        char summary[128];
        std::snprintf(summary, sizeof(summary),
                      "Invalidation of Block_%s revoked by %s",
                      invalidated_block_id.c_str(), revoked_by.c_str());
        e.summary = std::string(summary);
        op_logger_->Log(e, ctx);
    }
    return block;
}

Status MemoryExtractor::InvalidateMemory(const std::string& block_id,
                                         const std::string& reason,
                                         const observability::TraceContext* ctx) {
    if (!block_store_) {
        return Status::Internal("CX_ERR_MEMEXTRACT_INVALID_OUTPUT: no block store");
    }
    Result<MemoryBlockRecord> br = block_store_->GetMemoryBlock(block_id);
    if (!br.ok()) {
        return br.status();
    }
    MemoryBlockRecord block = br.value();
    if (!block.metadata_json.is_object()) {
        block.metadata_json = nlohmann::json::object();
    }
    const std::string now = NowIso8601();
    block.metadata_json["status"] = ToString(MemoryStatus::kInvalidated);
    block.metadata_json["invalidated_at"] = now;
    block.metadata_json["invalidation_triggered_by"] = "manual";
    block.metadata_json["invalidation_reason"] = reason;
    Status us = block_store_->UpdateMemoryBlock(block);
    if (!us.ok()) {
        return us;
    }
    MemoryExtractMetrics::Instance().RecordInvalidation(MemoryExtractMetrics::TriggeredBy::kManual);

    if (op_logger_) {
        observability::OperationLogEntry e;
        e.user_id = block.user_id.empty() ? "anonymous" : block.user_id;
        e.action = "memory_blocks_update";
        e.resource_type = "memory";
        e.resource_id = block_id;
        if (!block.ns_id.empty()) e.namespace_id = block.ns_id;
        char summary[128];
        std::snprintf(summary, sizeof(summary),
                      "Block_%s manually invalidated", block_id.c_str());
        e.summary = std::string(summary);
        op_logger_->Log(e, ctx);
    }
    return Status::Ok();
}

bool MemoryExtractor::IsPreferenceImmune(int existing_mention_count,
                                         const std::string& first_mentioned_at_iso,
                                         const std::string& now_iso,
                                         int threshold,
                                         int window_days) {
    // D8: a preference becomes immune once it has been mentioned >= threshold times
    // within the rolling window. This call represents one new mention, so the
    // effective count is existing + 1.
    const int effective_count = existing_mention_count + 1;
    if (effective_count < threshold) return false;

    // Window check: the first mention must be within window_days of now.
    const long long first = ParseIso8601ToEpoch(first_mentioned_at_iso);
    const long long now = ParseIso8601ToEpoch(now_iso);
    if (first < 0 || now < 0) {
        // Unknown timestamps → be conservative and treat as immune only on count
        // (window cannot be evaluated; count threshold already met).
        return true;
    }
    const long long window_secs = static_cast<long long>(window_days) * 24 * 60 * 60;
    return (now - first) <= window_secs;
}

}  // namespace cortrix::memory
