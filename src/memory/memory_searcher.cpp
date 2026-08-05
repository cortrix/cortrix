#include "cortrix/memory/memory_searcher.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "cortrix/memory/memory_isolation_metrics.h"

namespace cortrix {

MemorySearcher::MemorySearcher(QueryPipeline& pipeline,
                               MemoryStore& memory_store,
                               const MemoryConfig& config,
                               MemoryScorer* scorer)
    : pipeline_(pipeline)
    , memory_store_(memory_store)
    , config_(config)
    , scorer_(scorer) {}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

namespace {
// Isolation: user_id format rule — max 128 chars, ASCII printable (0x20-0x7E).
// Guards against injection / oversized identifiers (design § 5).
constexpr size_t kMaxUserIdLen = 128;
bool IsValidUserIdFormat(const std::string& user_id) {
    if (user_id.size() > kMaxUserIdLen) return false;
    for (unsigned char c : user_id) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// Parse an ISO 8601 "YYYY-MM-DDTHH:MM:SSZ" timestamp to Unix seconds.
// Returns 0 on a malformed / empty string — the scorer treats created_at <= 0
// as "unknown age" (age_days=0, decay_factor=1.0), i.e. no decay rather than a
// penalty (design § 5 boundary: created_at missing -> no decay).
int64_t ParseCreatedAtToEpoch(const std::string& iso) {
    if (iso.size() < 19) return 0;
    std::tm tm_utc{};
    int y, mo, d, h, mi, s;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
        return 0;
    }
    tm_utc.tm_year = y - 1900;
    tm_utc.tm_mon = mo - 1;
    tm_utc.tm_mday = d;
    tm_utc.tm_hour = h;
    tm_utc.tm_min = mi;
    tm_utc.tm_sec = s;
#if defined(_WIN32)
    return static_cast<int64_t>(_mkgmtime(&tm_utc));
#else
    return static_cast<int64_t>(timegm(&tm_utc));
#endif
}

// Read the memory block status ("active"/"tentative"/"invalidated") from
// item metadata. Missing/non-string -> "active" (design § 5: status missing ->
// treated as a valid/active state). ToMemoryResult does not surface status, so
// the scoring path reads it directly here.
std::string ExtractStatus(const json& metadata) {
    if (!metadata.is_null() && metadata.contains("status") &&
        metadata["status"].is_string()) {
        return metadata["status"].get<std::string>();
    }
    return "active";
}
}  // namespace

Status MemorySearchRequest::Validate() const {
    if (query.empty()) {
        return Status::InvalidArgument("query is required");
    }
    if (namespace_name.empty()) {
        return Status::InvalidArgument("namespace is required");
    }
    // user_id is always required for memory isolation.
    if (user_id.empty()) {
        return Status::InvalidArgument("user_id is required for memory isolation");
    }
    // Enforce user_id format (length + ASCII printable).
    if (!IsValidUserIdFormat(user_id)) {
        return Status::InvalidArgument("invalid user_id format");
    }
    if (scope == MemoryScope::kSession && session_id.empty()) {
        return Status::InvalidArgument("session_id is required for scope=session");
    }
    if (top_k < 1 || top_k > 100) {
        return Status::InvalidArgument("top_k must be between 1 and 100");
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

MemorySearchResponse MemorySearcher::Search(const MemorySearchRequest& request) {
    MemorySearchResponse response;

    auto start = std::chrono::steady_clock::now();

    // 1. Validate
    auto s = request.Validate();
    if (!s.ok()) {
        spdlog::warn("MemorySearch validation failed: {}", s.message());
        return response;
    }

    // 2. Convert to QueryRequest
    QueryRequest qr = ToQueryRequest(request);

    // 3. Execute via QueryPipeline
    QueryResponse qresp = pipeline_.Execute(qr);

    response.degraded = qresp.meta.degraded;

    // 4. Post-filter: scope + TTL. Collect all surviving rows first (no early
    //    top_k cut) — the scorer re-ranks by final_score below, so truncation must
    //    happen after scoring, not in pipeline (raw RRF) order.
    std::vector<MemorySearchResultItem> kept;
    std::vector<MemoryCandidate> candidates;  // parallel to `kept` (scorer path)
    kept.reserve(qresp.results.size());
    candidates.reserve(qresp.results.size());

    for (const auto& item : qresp.results) {
        // 4a. Scope filter (always applied — items without user_id in
        //     metadata, including null metadata, fail the user_id check and are
        //     excluded. MatchScope handles null/missing metadata via contains()).
        if (!MatchScope(item.metadata, request)) {
            continue;
        }

        // 4b. TTL expiry check
        std::string meta_str = item.metadata.is_null() ? "" : item.metadata.dump();
        bool expired = IsExpired(meta_str);
        if (expired && !request.include_expired) {
            continue;
        }

        // 5. Convert to MemorySearchResultItem
        MemorySearchResultItem result = ToMemoryResult(item);
        result.expired = expired;
        // Default (null-scorer / no-decay): final_score mirrors the raw RRF score
        // so the field is always meaningful and ordering is unchanged.
        result.final_score = static_cast<double>(result.score);

        // Keep interaction rows (interaction_id) AND extracted fact/preference/event
        // blocks (content). A result with neither is a non-memory row that slipped
        // the MEMORY block_type filter — drop it.
        if (result.interaction_id.empty() && result.content.empty()) {
            continue;
        }

        // 5b. Scorer path only: build the scoring candidate parallel to
        //     `kept`. block_id is repurposed as a back-correlation index into
        //     `kept` (real block ids are ULID strings carried on the result item;
        //     the scorer only passes this field through and never matches on it).
        //     status is read from metadata (ToMemoryResult does not surface it).
        if (scorer_ != nullptr) {
            MemoryCandidate cand;
            cand.block_id = candidates.size();  // index handle, not a real id
            cand.content = result.content;
            cand.memory_type = result.memory_type;
            cand.status = ExtractStatus(item.metadata);
            cand.raw_score = static_cast<double>(result.score);
            cand.created_at = ParseCreatedAtToEpoch(result.created_at);
            candidates.push_back(std::move(cand));
        }

        kept.push_back(std::move(result));
    }

    if (scorer_ != nullptr) {
        // Scorer path: classified-decay scoring + invalidated filter + rank by
        // final_score + top_k truncation (all in MemoryScorer::ScoreAndRank,
        // design § 4.1 step 4). Map each ScoredMemory back to its kept row via
        // the index stashed in block_id, writing final_score / decay_factor.
        const int64_t now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::vector<ScoredMemory> scored = scorer_->ScoreAndRank(
            candidates, now, request.top_k, request.include_invalidated);
        response.results.reserve(scored.size());
        for (const auto& sm : scored) {
            size_t idx = static_cast<size_t>(sm.block_id);
            if (idx >= kept.size()) continue;  // defensive; should not happen
            MemorySearchResultItem result = std::move(kept[idx]);
            result.decay_factor = sm.decay_factor;
            result.final_score = sm.final_score;
            response.results.push_back(std::move(result));
        }
    } else {
        // Null-scorer fallback (pre-scorer behavior): keep raw RRF order, truncate
        // to top_k by count. final_score already mirrors score (set above).
        for (auto& result : kept) {
            response.results.push_back(std::move(result));
            if (static_cast<int>(response.results.size()) >= request.top_k) {
                break;
            }
        }
    }

    response.total_results = static_cast<int>(response.results.size());

    auto end = std::chrono::steady_clock::now();
    response.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    return response;
}

// ---------------------------------------------------------------------------
// ToQueryRequest
// ---------------------------------------------------------------------------

QueryRequest MemorySearcher::ToQueryRequest(const MemorySearchRequest& request) const {
    QueryRequest qr;
    qr.query = request.query;
    qr.namespace_name = request.namespace_name;
    qr.top_k = request.top_k * 2;  // oversample for TTL filter
    qr.timeout_ms = request.timeout_ms;
    qr.filters.block_types = {"MEMORY"};
    qr.search_config.enable_sql = false;
    qr.search_config.enable_vector = true;
    qr.search_config.enable_bm25 = true;
    return qr;
}

// ---------------------------------------------------------------------------
// IsExpired
// ---------------------------------------------------------------------------

bool MemorySearcher::IsExpired(const std::string& metadata_json) {
    if (metadata_json.empty()) return false;

    try {
        auto meta = nlohmann::json::parse(metadata_json);

        int ttl_seconds = 0;
        if (meta.contains("ttl_seconds") && meta["ttl_seconds"].is_number()) {
            ttl_seconds = meta["ttl_seconds"].get<int>();
        }

        if (ttl_seconds <= 0) return false;  // never expires

        if (!meta.contains("queried_at") || !meta["queried_at"].is_string()) {
            return false;  // no timestamp, treat as not expired (lenient)
        }

        std::string queried_at = meta["queried_at"].get<std::string>();

        // Parse ISO 8601 timestamp
        std::tm tm = {};
        int ms = 0;
        if (std::sscanf(queried_at.c_str(),
                        "%d-%d-%dT%d:%d:%d.%dZ",
                        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) < 6) {
            return false;  // parse failure, treat as not expired
        }
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;

        auto created_time = timegm(&tm);
        auto expiry_time = created_time + ttl_seconds;

        auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());

        return now > expiry_time;

    } catch (const std::exception& e) {
        spdlog::warn("TTL metadata parse failed: {}", e.what());
        return false;  // parse failure, treat as not expired (lenient)
    }
}

// ---------------------------------------------------------------------------
// MatchScope
// ---------------------------------------------------------------------------

bool MemorySearcher::MatchScope(const json& metadata,
                                const MemorySearchRequest& request) const {
    // ALWAYS filter by user_id first — this is the core isolation line.
    // A memory with no user_id (or wrong user_id) is excluded on every path.
    // §8.bis: each exclusion is a match_scope_excluded_total{reason} event (the
    // retrieval-path pre-filter, distinct from the API-entry isolation_violation).
    if (!metadata.contains("user_id") || !metadata["user_id"].is_string()) {
        memory::MemoryIsolationMetrics::Instance().RecordMatchScopeExcluded(
            memory::MemoryIsolationMetrics::Reason::kMissingUserId);
        return false;  // no user_id in metadata -> exclude
    }
    if (metadata["user_id"].get<std::string>() != request.user_id) {
        memory::MemoryIsolationMetrics::Instance().RecordMatchScopeExcluded(
            memory::MemoryIsolationMetrics::Reason::kMismatch);
        return false;  // wrong user -> exclude
    }

    // Then apply the scope-specific filter.
    switch (request.scope) {
        case MemoryScope::kSession: {
            if (!metadata.contains("session_id") || !metadata["session_id"].is_string()) {
                return false;
            }
            return metadata["session_id"].get<std::string>() == request.session_id;
        }
        case MemoryScope::kUser:
        default:
            return true;  // user_id already matched above
    }
}

// ---------------------------------------------------------------------------
// ToMemoryResult
// ---------------------------------------------------------------------------

MemorySearchResultItem MemorySearcher::ToMemoryResult(const ResultItem& item) const {
    MemorySearchResultItem result;
    result.score = item.score;
    result.created_at = "";

    // The unified memory read pipeline surfaces two row kinds from the MEMORY
    // block_type: interaction rows (interaction_id + Q/A) and extracted
    // fact/preference/event blocks (memory_type + block_id, content_text = the fact
    // statement). Pull whichever metadata fields are present.
    if (!item.metadata.is_null()) {
        const auto& m = item.metadata;
        if (m.contains("interaction_id") && m["interaction_id"].is_string())
            result.interaction_id = m["interaction_id"].get<std::string>();
        if (m.contains("session_id") && m["session_id"].is_string())
            result.session_id = m["session_id"].get<std::string>();
        if (m.contains("query_type") && m["query_type"].is_string())
            result.query_type = m["query_type"].get<std::string>();
        if (m.contains("memory_type") && m["memory_type"].is_string())
            result.memory_type = m["memory_type"].get<std::string>();
        if (m.contains("block_id") && m["block_id"].is_string())
            result.block_id = m["block_id"].get<std::string>();
        if (m.contains("created_at") && m["created_at"].is_string())
            result.created_at = m["created_at"].get<std::string>();
        result.metadata_json = m.dump();
    }

    if (!result.interaction_id.empty()) {
        // Interaction row — chunk_text is "{query}\n---\n{response}".
        auto sep = item.chunk_text.find("\n---\n");
        if (sep != std::string::npos) {
            result.query_text = item.chunk_text.substr(0, sep);
            result.response_text = item.chunk_text.substr(sep + 5);
        } else {
            result.query_text = item.chunk_text;
        }
    } else {
        // Fact/preference/event block — content_text is the fact statement
        // itself (no Q/A split). This is what makes extracted memories retrievable
        // through /memory/search (the unified read pipeline).
        result.content = item.chunk_text;
    }

    return result;
}

}  // namespace cortrix
