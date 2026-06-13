#include "cortrix/memory/memory_searcher.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <ctime>

#include "cortrix/memory/mem05_metrics.h"

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
// MEM05: user_id format rule — max 128 chars, ASCII printable (0x20-0x7E).
// Guards against injection / oversized identifiers (design § 5).
constexpr size_t kMaxUserIdLen = 128;
bool IsValidUserIdFormat(const std::string& user_id) {
    if (user_id.size() > kMaxUserIdLen) return false;
    for (unsigned char c : user_id) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}
}  // namespace

Status MemorySearchRequest::Validate() const {
    if (query.empty()) {
        return Status::InvalidArgument("query is required");
    }
    if (namespace_name.empty()) {
        return Status::InvalidArgument("namespace is required");
    }
    // MEM05: user_id is always required for memory isolation.
    if (user_id.empty()) {
        return Status::InvalidArgument("user_id is required for memory isolation");
    }
    // MEM05: enforce user_id format (length + ASCII printable).
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

    // 4. Post-filter: scope + TTL
    for (const auto& item : qresp.results) {
        // 4a. Scope filter (MEM05: always applied — items without user_id in
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

        // Keep interaction rows (interaction_id) AND MEM02 fact/preference/event
        // blocks (content). A result with neither is a non-memory row that slipped
        // the MEMORY block_type filter — drop it.
        if (!result.interaction_id.empty() || !result.content.empty()) {
            response.results.push_back(std::move(result));
        }

        if (static_cast<int>(response.results.size()) >= request.top_k) {
            break;
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
    // MEM05: ALWAYS filter by user_id first — this is the core isolation line.
    // A memory with no user_id (or wrong user_id) is excluded on every path.
    // §8.bis: each exclusion is a match_scope_excluded_total{reason} event (the
    // retrieval-path pre-filter, distinct from the API-entry isolation_violation).
    if (!metadata.contains("user_id") || !metadata["user_id"].is_string()) {
        memory::Mem05Metrics::Instance().RecordMatchScopeExcluded(
            memory::Mem05Metrics::Reason::kMissingUserId);
        return false;  // no user_id in metadata -> exclude
    }
    if (metadata["user_id"].get<std::string>() != request.user_id) {
        memory::Mem05Metrics::Instance().RecordMatchScopeExcluded(
            memory::Mem05Metrics::Reason::kMismatch);
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
    // block_type: interaction rows (interaction_id + Q/A) and MEM02
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
        // MEM02 fact/preference/event block — content_text is the fact statement
        // itself (no Q/A split). This is what makes extracted memories retrievable
        // through /memory/search (the unified read pipeline, MEM02 §6.5.3).
        result.content = item.chunk_text;
    }

    return result;
}

}  // namespace cortrix
