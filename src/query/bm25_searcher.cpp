#include "cortrix/query/bm25_searcher.h"
#include <chrono>
#include <string>
#include <vector>

namespace cortrix {

BM25Searcher::BM25Searcher(CortrixStore& store)
    : store_(store) {}

RouteResult BM25Searcher::Search(const std::string& query_text, int top_k, int64_t timeout_us) {
    RouteResult result;
    result.route_name = "bm25";

    auto start = std::chrono::steady_clock::now();

    // Sanitize query for FTS5
    std::string sanitized = SanitizeFtsQuery(query_text);
    if (sanitized.empty()) {
        result.status = RouteStatus::kError;
        result.error_message = "empty query after sanitization";
        return result;
    }

    // Search with oversampling (top_k * 3)
    int search_k = top_k * 3;
    std::vector<SearchResult> search_results;
    int rc = store_.search_fulltext(sanitized, search_k, search_results);

    auto elapsed = std::chrono::steady_clock::now() - start;
    result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    // Check timeout
    if (result.latency_us > timeout_us) {
        result.status = RouteStatus::kTimeout;
        result.error_message = "bm25 search timeout";
        return result;
    }

    if (rc != 0) {
        result.status = RouteStatus::kError;
        result.error_message = "fulltext search failed";
        return result;
    }

    // Convert SearchResult -> RouteResultItem
    result.items.reserve(search_results.size());
    for (const auto& sr : search_results) {
        RouteResultItem item;
        item.block_id = sr.block_id;
        item.doc_id = sr.doc_id;
        item.chunk_index = 0;
        item.raw_score = static_cast<float>(sr.score);
        result.items.push_back(item);
    }

    result.status = RouteStatus::kSuccess;
    return result;
}

std::string BM25Searcher::SanitizeFtsQuery(const std::string& raw) {
    std::string result;
    result.reserve(raw.size());

    // FTS5 boolean/proximity operators that must be stripped when they appear
    // as standalone tokens to prevent query injection.  We check case-sensitively
    // because FTS5 operators are uppercase-only.
    static const char* fts5_operators[] = {
        "AND", "OR", "NOT", "NEAR",
    };

    // First pass: strip special characters
    std::string stripped;
    stripped.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        switch (c) {
            case '"':
                stripped += "\"\"";  // Escape double quotes
                break;
            case '*':
                // Remove asterisks (prevent prefix query abuse)
                break;
            case '(':
            case ')':
                // Remove parentheses (prevent FTS5 expression injection)
                break;
            case '^':
            case ':':
                // Remove column filter and initial-token operators
                stripped += ' ';
                break;
            default:
                stripped += c;
                break;
        }
    }

    // Second pass: remove standalone FTS5 boolean operators (AND, OR, NOT, NEAR)
    // We split by whitespace, filter operators, and rejoin.
    std::vector<std::string> tokens;
    {
        size_t pos = 0;
        while (pos < stripped.size()) {
            // skip whitespace
            while (pos < stripped.size() && (stripped[pos] == ' ' || stripped[pos] == '\t' || stripped[pos] == '\n' || stripped[pos] == '\r'))
                ++pos;
            if (pos >= stripped.size()) break;
            size_t start = pos;
            while (pos < stripped.size() && stripped[pos] != ' ' && stripped[pos] != '\t' && stripped[pos] != '\n' && stripped[pos] != '\r')
                ++pos;
            tokens.push_back(stripped.substr(start, pos - start));
        }
    }

    // Filter out standalone FTS5 operators and strip leading +/- prefix operators.
    // FTS5 treats leading '+' as "required" and '-' as "exclude"; we strip them
    // to prevent query injection while preserving the rest of the token.
    std::vector<std::string> filtered;
    filtered.reserve(tokens.size());
    for (const auto& token : tokens) {
        bool is_operator = false;
        for (const auto* op : fts5_operators) {
            if (token == op) {
                is_operator = true;
                break;
            }
        }
        if (is_operator) continue;

        // Also strip NEAR/N forms (e.g., "NEAR/5")
        if (token.size() >= 5 && token.compare(0, 5, "NEAR/") == 0) {
            continue;
        }

        // Strip leading '+' or '-' prefix operators
        std::string cleaned = token;
        while (!cleaned.empty() && (cleaned.front() == '+' || cleaned.front() == '-')) {
            cleaned.erase(cleaned.begin());
        }
        if (!cleaned.empty()) {
            filtered.push_back(cleaned);
        }
    }

    // Rejoin
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (i > 0) result += ' ';
        result += filtered[i];
    }

    return result;
}

}  // namespace cortrix
