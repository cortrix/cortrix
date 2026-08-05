#pragma once
#include <string>

namespace cortrix::reranker {

/// Input length category. Thresholds are expressed in tokens
/// relative to max_seq_length (default 512); the EXTREMELY_LONG cutoff is
/// 4×max_seq_length (default 2048). These are tokenizer-agnostic — the caller
/// supplies token counts (real HfTokenizer counts in production, an approximate
/// proxy in stub mode).
enum class InputCategory {
    kNormal,         ///< passage_tokens ≤ max_seq_length - query_tokens - 3
    kTruncated,      ///< above NORMAL but passage_tokens ≤ 4×max_seq_length
    kExtremelyLong,  ///< passage_tokens > 4×max_seq_length
};

/// Reserved special tokens in a cross-encoder pair input ([CLS] q [SEP] p [SEP]).
/// (§3.3 "max_seq_length - query - 3 tokens".)
inline constexpr int kReservedSpecialTokens = 3;

/// Categorize one query-passage pair by length (the inner loop's
/// first step). Pure function of token counts (no tokenizer dependency).
///
/// NORMAL when passage_tokens ≤ max_seq_length - query_tokens - 3. If the query
/// alone already fills (or overflows) the window, the NORMAL budget is ≤ 0 so any
/// non-empty passage escalates — that itself is a config-alignment problem the
/// startup check (S3.2) guards against, but the runtime path still degrades
/// gracefully (truncate / score=0) rather than throwing.
InputCategory CategorizeInput(int query_tokens, int passage_tokens, int max_seq_length);

/// The token budget left for the passage in NORMAL inputs
/// (max_seq_length - query_tokens - 3, floored at 0).
int PassageTokenBudget(int query_tokens, int max_seq_length);

/// Result of preparing one passage for scoring.
struct PreprocessedPassage {
    InputCategory category;
    std::string   text;          ///< NORMAL: original; TRUNCATED: front-truncated; EXTREMELY_LONG: ""
    bool          force_zero;    ///< true for EXTREMELY_LONG → caller sets score=0 (does NOT score)
};

/// InputPreprocessor — applies the §3.3 three-segment policy to a passage and
/// drives the truncated_total / extremely_long_total metrics + WARN/ERROR logs.
///
/// Standalone (D3): token counting uses an injected counter (real HfTokenizer
/// in S-W later / production; a byte-based proxy by default so it is testable
/// offline). The query keeps its full length (§7 DoD: "truncation keeps the full query length +
/// the front of the passage"); only the passage is truncated.
class InputPreprocessor {
public:
    /// Approximate token count proxy used when no real tokenizer is wired
    /// (consistent with the SPC recursive_chunker heuristic): ~1 token / 4 bytes,
    /// min 1 for non-empty text. Public so tests can predict categories.
    static int ApproxTokenCount(const std::string& text);

    /// Prepare `passage` for scoring against `query` under `max_seq_length`,
    /// recording metrics + logging per category:
    ///   NORMAL         → {kNormal, passage, false}
    ///   TRUNCATED      → {kTruncated, <front-truncated>, false} + LOG_WARN +
    ///                    cortrix_reranker_truncated_total++
    ///   EXTREMELY_LONG → {kExtremelyLong, "", true} + LOG_ERROR +
    ///                    cortrix_reranker_extremely_long_total++
    /// Token counts use ApproxTokenCount (the stub/offline path).
    static PreprocessedPassage Prepare(const std::string& query,
                                       const std::string& passage,
                                       int max_seq_length);

    /// UTF-8-safe front truncation of `text` to at most `max_tokens` tokens
    /// (using the ApproxTokenCount heuristic's inverse ~4 bytes/token), never
    /// splitting a multi-byte code point. Exposed for testing.
    static std::string TruncateToTokens(const std::string& text, int max_tokens);
};

}  // namespace cortrix::reranker
