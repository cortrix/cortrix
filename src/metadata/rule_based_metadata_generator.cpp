#include "cortrix/metadata/rule_based_metadata_generator.h"

#include <chrono>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#include "cortrix/id/ulid.h"
#include "cortrix/metadata/metadata_error.h"
#include "cortrix/metadata/metadata_metrics.h"

namespace cortrix::metadata {

namespace {

// Format a Unix-epoch-ms timestamp as ISO-8601 UTC ("2026-04-02T10:30:00Z"), the
// schema upload_time shape (detailed design §3.1). 0 (unset) → empty string so the caller can
// fall back to null. Uses gmtime (UTC) — the wire contract is always Z-suffixed UTC.
std::string FormatIso8601Utc(int64_t epoch_ms) {
    if (epoch_ms <= 0) return "";
    std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

// doc_title derivation (§9.quater.2): filename with the last extension stripped.
// "report.pdf" → "report"; no-dot / leading-dot names (".gitignore") are returned unchanged.
std::string DeriveDocTitle(const std::string& filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return filename;
    return filename.substr(0, dot);
}

// Join tags with single spaces (topics_rule_extracted derivation §9.quater.2 + block_text
// tags rendering). Empty vector → empty string.
std::string JoinTags(const std::vector<std::string>& tags, const char* sep) {
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out += sep;
        out += tags[i];
    }
    return out;
}

}  // namespace

std::string RuleBasedMetadataGenerator::BuildBlockText(const GeneratorInput& input) {
    // D3 lock: a natural-language sentence over the 7-10 core fields (detailed design §3.2):
    // filename / mime_type / page_count / upload_time / lang / tags / parser. Deterministic
    // ordering + only-present clauses so the same input always embeds identically and
    // missing fields don't inject empty noise.
    const auto& dm = input.doc_metadata;
    std::vector<std::string> parts;

    if (!dm.filename.empty()) parts.push_back(dm.filename);
    if (!dm.mime_type.empty()) parts.push_back(dm.mime_type + " document");
    if (dm.page_count > 0) parts.push_back(std::to_string(dm.page_count) + " pages");
    if (input.stats.chunk_count > 0)
        parts.push_back(std::to_string(input.stats.chunk_count) + " chunks");

    std::string upload = FormatIso8601Utc(input.file_info.upload_time_ms);
    if (!upload.empty()) parts.push_back(upload + " uploaded");

    if (!dm.doc_language.empty()) parts.push_back(dm.doc_language);
    if (!input.stats.parser_name.empty()) parts.push_back("parser: " + input.stats.parser_name);
    if (!input.file_info.tags.empty())
        parts.push_back("tags: " + JoinTags(input.file_info.tags, " / "));

    return JoinTags(parts, ", ");
}

nlohmann::json RuleBasedMetadataGenerator::BuildMetadataJson(
    const GeneratorInput& input, std::vector<std::string>* missing_fields) {
    const auto& dm = input.doc_metadata;
    nlohmann::json j;

    // --- identity / system fields (class A, detailed design §3.1 / §3.3) ---
    j["block_id"] = nullptr;  // filled by Generate after minting the ULID
    j["block_type"] = "metadata";
    j["doc_id"] = input.doc_id;
    j["namespace_id"] = input.namespace_id;

    // --- file facts (from F06 DocumentMetadata + F08 FileInfo) ---
    j["filename"] = dm.filename;
    j["mime_type"] = dm.mime_type;
    j["file_size_bytes"] = dm.file_size_bytes;
    std::string upload = FormatIso8601Utc(input.file_info.upload_time_ms);
    j["upload_time"] = upload.empty() ? nlohmann::json(nullptr) : nlohmann::json(upload);

    // --- counts (page from F06; chunk/parent/child from F34 via ProcessingStats) ---
    // page_count unknown (§5.2 row 2: F06 partial success) → null + warning.
    if (dm.page_count >= 0) {
        j["page_count"] = dm.page_count;
    } else {
        j["page_count"] = nullptr;
        if (missing_fields) missing_fields->push_back("page_count");
    }
    j["chunk_count"] = input.stats.chunk_count >= 0 ? nlohmann::json(input.stats.chunk_count)
                                                    : nlohmann::json(nullptr);
    j["parent_count"] = input.stats.parent_count >= 0 ? nlohmann::json(input.stats.parent_count)
                                                      : nlohmann::json(nullptr);
    j["child_count"] = input.stats.child_count >= 0 ? nlohmann::json(input.stats.child_count)
                                                    : nlohmann::json(nullptr);

    // --- provenance (parser / enricher / chunker + versions) ---
    j["parser"] = input.stats.parser_name;
    j["parser_version"] = input.stats.parser_version;
    j["enricher"] = input.stats.enricher_name;
    j["enricher_version"] = input.stats.enricher_version;
    j["chunker"] = input.stats.chunker_name;
    j["chunker_version"] = input.stats.chunker_version;

    // --- processing + source + lang ---
    j["processing_time_ms"] = input.stats.processing_time_ms;
    j["source_uri"] = input.file_info.source_uri;
    // lang (schema key `lang`) ← F06 doc_language (consumed field name differs, briefing §2).
    j["lang"] = dm.doc_language;

    // --- F10 coordination signals (V2 remediation M-03, class A immutable, F06 → F08 passthrough) ---
    std::string parse_status = input.stats.parse_status.empty() ? "ok" : input.stats.parse_status;
    j["meta.parse_status"] = parse_status;
    // meta.parse_failed_page: int? — non-null only when failed/partial AND a page is set.
    if ((parse_status == "failed" || parse_status == "partial") &&
        input.stats.parse_failed_page >= 0) {
        j["meta.parse_failed_page"] = input.stats.parse_failed_page;
    } else {
        j["meta.parse_failed_page"] = nullptr;
    }

    // --- business fields (class B, V1.0 set once at upload time + immutable, D9 B' lock) ---
    j["tags"] = input.file_info.tags;  // array (possibly empty)
    // ingestion_status: derived from the F06 parse status (completed unless failed).
    j["ingestion_status"] = (parse_status == "failed") ? "failed" : "completed";
    // custom_metadata: business-supplied object (default {}).
    j["custom_metadata"] = input.custom_metadata.is_null() ? nlohmann::json::object()
                                                           : input.custom_metadata;

    return j;
}

nlohmann::json RuleBasedMetadataGenerator::DeriveDocFts5Columns(const GeneratorInput& input) {
    // §9.quater.2 V1.0 derivation rules. Pure logical mapping — zero F08 schema change, no
    // F41 dependency. The future F41 SchemaProvider.InsertDocFts5 reads these.
    nlohmann::json cols;
    cols["doc_id"] = input.doc_id;
    cols["filename"] = input.doc_metadata.filename;
    cols["doc_title"] = DeriveDocTitle(input.doc_metadata.filename);
    cols["topics_rule_extracted"] = JoinTags(input.file_info.tags, " ");
    // authors: business may self-fill custom_metadata.authors (D9 B' lock); else null.
    if (input.custom_metadata.is_object() && input.custom_metadata.contains("authors")) {
        cols["authors"] = input.custom_metadata["authors"];
    } else {
        cols["authors"] = nullptr;
    }
    return cols;
}

Result<GeneratorOutput> RuleBasedMetadataGenerator::Generate(
    const GeneratorInput& input, const observability::TraceContext* /*ctx*/) {
    auto& metrics = MetadataMetrics::Instance();

    // §5.2 row 1: F06 parse fully failed → no DocumentMetadata → cannot fallback → GEN_FAILED.
    // "Completely empty" = no filename, no source_uri, and the parse status is failed.
    const auto& dm = input.doc_metadata;
    const bool no_identity = dm.filename.empty() && input.file_info.source_uri.empty();
    const bool parse_failed = input.stats.parse_status == "failed";
    if (no_identity && parse_failed) {
        metrics.RecordGenerated(MetadataMetrics::GenStatus::kFailed,
                                MetadataMetrics::GenSource::kApiUpload);
        nlohmann::json sd = {
            {"doc_id", input.doc_id},
            {"namespace_id", input.namespace_id},
            {"stage", "block_text_assembly"},
            {"missing_fields", {"doc_metadata", "page_count", "chunk_count"}},
            {"f06_parse_status", "failed"},
            {"downstream_blocked", {"f34_chunker", "f25_pwl"}},
        };
        return MetadataStatus(
            MetadataErrorCode::kGenFailed,
            "Failed to generate metadata block: doc_metadata is empty (F06 parse failed)");
    }

    // §5.bis cortrix_f08_block_generate_duration_seconds — time block_text assembly +
    // JSON serialization (steady_clock, immune to wall-clock jumps). Mirrors F07
    // ObserveAssignDuration (commit d634fdb): the histogram was defined/rendered/tested
    // but Generate() never fed it. The GEN_FAILED early return above does no assembly, so
    // it is intentionally outside this timer (SLA P95 ≤ 50ms covers the produced-block path).
    const auto start = std::chrono::steady_clock::now();

    GeneratorOutput out;
    out.block.block_id = cortrix::id::GenerateUlid();
    out.block.doc_id = input.doc_id;
    out.block.namespace_id = input.namespace_id;
    out.block.block_text = BuildBlockText(input);
    out.block.metadata_json = BuildMetadataJson(input, &out.missing_fields);
    out.block.metadata_json["block_id"] = out.block.block_id;
    // embedding(block_text) + P-HNSW insert = D3.5 pipeline wiring → left empty here.

    for (const std::string& f : out.missing_fields) {
        metrics.RecordFieldMissing(f);
    }
    metrics.ObserveMetadataSize(out.block.metadata_json.dump().size());
    metrics.RecordGenerated(out.missing_fields.empty()
                                ? MetadataMetrics::GenStatus::kSuccess
                                : MetadataMetrics::GenStatus::kPartialWarning,
                            MetadataMetrics::GenSource::kApiUpload);

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    metrics.ObserveGenerateDuration(elapsed.count());
    return out;
}

}  // namespace cortrix::metadata
