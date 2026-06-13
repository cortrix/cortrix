#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/retrieval/types.h"  // RankedChunk (F02 output / F37 input)

namespace cortrix::retrieval {

/// IClassifier — the shared post-retrieval classifier abstraction (F37 §5.1, S1
/// ruling of the G3+G1.2 joint D1). F37 CragEvaluator, F39 QueryComplexityClassifier
/// (C-R2), and the existing IntentClassifier all implement this one interface so
/// the query pipeline can treat "classify a retrieval/query signal" uniformly.
///
/// Standalone note (D3): F37 builds CragEvaluator : IClassifier against this header
/// + a mock/heuristic backend. F39's QueryComplexityClassifier is NOT frozen this
/// round and reuses this interface in C-R2 (the briefing's "F39 reuses your
/// IClassifier"); nothing here depends on F39.

/// Input to a classifier: the query plus the F02-reranked candidates it is judging.
struct ClassifierInput {
    std::string query;
    std::vector<RankedChunk> chunks;     ///< F02 Reranker output (chunks + scores)
    std::optional<std::string> ns_id;    ///< target namespace (for NS-config resolution)
};

/// Result of one classification.
struct ClassificationResult {
    std::string label;                   ///< CragEvaluator: "correct" / "ambiguous" / "incorrect"
    float score = 0.0f;                  ///< primary score (CRAG: softmax confidence of the verdict)
    float confidence = 0.0f;             ///< classifier confidence (drives heuristic-guard fallback)
    std::map<std::string, float> raw_signals;  ///< multi-signal feature dump (top1 / median / std / ...)
    std::optional<std::string> error_code;     ///< CX_ERR_F37_* token when a fallback path was taken
};

/// The shared classifier contract.
class IClassifier {
public:
    virtual ~IClassifier() = default;

    /// Classify one input. Implementations must be total (never throw across this
    /// boundary): a backend failure is reported via ClassificationResult.error_code
    /// + a safe fallback label, per F37 §6.2 / §7.3.
    virtual ClassificationResult Classify(const ClassifierInput& input) = 0;

    /// Stable classifier name (logging / explain endpoint). e.g. "f37-crag".
    virtual std::string Name() const = 0;

    /// The full set of labels this classifier can emit (stable, ordered).
    virtual std::vector<std::string> Labels() const = 0;

    /// True when the classifier backend is loaded and usable. When false, callers
    /// skip it (L1/L2 fallback) rather than calling Classify().
    virtual bool IsAvailable() const = 0;
};

}  // namespace cortrix::retrieval
