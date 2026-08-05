#include <cstdint>
#include "cortrix/query/intent_classifier.h"
#include <cctype>

namespace cortrix {

IntentClassifier::IntentClassifier(const LlmConfig& llm_config)
    : llm_config_(llm_config)
    , llm_enabled_(!llm_config.provider.empty() && !llm_config.api_key.empty()) {}

QueryIntent IntentClassifier::Classify(const std::string& query_text, int64_t timeout_us) {
    if (llm_enabled_) {
        // LLM classification with automatic fallback to keyword mode on failure.
        // This ensures intent classification never blocks the query pipeline.
        try {
            return ClassifyByLlm(query_text, timeout_us);
        } catch (...) {
            // LLM call failed (network error, parse error, etc.); degrade gracefully
            return ClassifyByKeyword(query_text);
        }
    }
    return ClassifyByKeyword(query_text);
}

const char* IntentClassifier::IntentToLabel(QueryIntent intent) {
    switch (intent) {
        case QueryIntent::kSemantic: return "semantic";
        case QueryIntent::kSql:      return "sql";
        case QueryIntent::kHybrid:   return "hybrid";
    }
    return "semantic";  // unreachable for a valid enum; defensive default
}

retrieval::ClassificationResult IntentClassifier::Classify(
    const retrieval::ClassifierInput& input) {
    // S2-D adapter: delegate to the existing intent logic (which already degrades to
    // keyword matching on any LLM fault, so this is total). A 1s default timeout is
    // used since the IClassifier signature carries none; the LLM stub ignores it.
    // input.chunks is intentionally unused — intent is a pre-retrieval signal.
    const QueryIntent intent = Classify(input.query, /*timeout_us=*/1'000'000);
    retrieval::ClassificationResult r;
    r.label = IntentToLabel(intent);
    r.score = 1.0f;       // Phase 1 keyword/LLM intent has no calibrated score
    r.confidence = 1.0f;
    return r;
}

bool IntentClassifier::IsLlmEnabled() const {
    return llm_enabled_;
}

QueryIntent IntentClassifier::ClassifyByLlm(const std::string& query_text, int64_t /*timeout_us*/) {
    // MVP STUB: LLM-based classification is not implemented in MVP.
    // In Phase 2, this method will call an LLM API (e.g., OpenAI, Claude) with a prompt
    // that classifies the query into one of three intents: kSemantic, kSql, kHybrid.
    // Until then, we fall back to keyword heuristics, which is safe because:
    //   - The caller (Classify) wraps this in a try/catch for robustness
    //   - Keyword classification provides reasonable defaults
    //   - the SQL route is skipped anyway, so misclassification has no effect in MVP
    return ClassifyByKeyword(query_text);
}

QueryIntent IntentClassifier::ClassifyByKeyword(const std::string& query_text) {
    // Convert to uppercase for case-insensitive matching
    std::string upper;
    upper.reserve(query_text.size());
    for (char c : query_text) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    // SQL keyword set (English)
    // Design spec: Text-to-SQL only generates SELECT statements.
    // INSERT/UPDATE/DELETE are explicitly prohibited (see text-to-sql-implementation.md
    // Section 4: SQL safety allowlist).  We must NOT route queries containing DML keywords
    // to the SQL path -- they should fall through to semantic search.
    static const char* sql_keywords[] = {
        "SELECT", "FROM", "WHERE",
        "JOIN", "GROUP BY", "ORDER BY", "HAVING", "LIMIT",
        "COUNT", "SUM", "AVG", "MAX", "MIN",
    };

    for (const auto* kw : sql_keywords) {
        if (upper.find(kw) != std::string::npos) {
            return QueryIntent::kSql;
        }
    }

    // Chinese SQL-related keywords (check original text for multi-byte chars).
    // The UTF-8 bytes below are functional matching data, not display text — they
    // MUST stay Chinese so Chinese-language queries route to the SQL path. The
    // trailing comment is an English gloss of each keyword.
    static const char* cn_keywords[] = {
        "\xe8\xa1\xa8",       // "table"
        "\xe6\x9f\xa5\xe8\xaf\xa2", // "query"
        "\xe7\xbb\x9f\xe8\xae\xa1", // "statistics"
        "\xe6\xb1\x87\xe6\x80\xbb", // "summary/aggregate"
        "\xe6\x8e\x92\xe5\x90\x8d", // "ranking"
        "\xe5\xa4\x9a\xe5\xb0\x91", // "how many"
        "\xe5\x87\xa0\xe4\xb8\xaa", // "how many (count)"
    };

    for (const auto* kw : cn_keywords) {
        if (query_text.find(kw) != std::string::npos) {
            return QueryIntent::kSql;
        }
    }

    return QueryIntent::kSemantic;
}

}  // namespace cortrix
