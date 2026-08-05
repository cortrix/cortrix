#pragma once
#include <memory>
#include <string>

#include "cortrix/common/status.h"
#include "cortrix/spc/hf_tokenizer.h"

namespace cortrix::ml {

/// Process-wide HfTokenizer registry (the lifecycle SoT).
///
/// Background: the reranker, sparse retrieval and the embedder share the bge-m3 tokenizer.
/// Loading it once and sharing the shared_ptr avoids per-component reload cost
/// ("eager load (avoids runtime tokenizer loading cost)").
///
/// Thread-safety: registration takes a std::mutex; reads after registration are
/// lock-free against the returned shared_ptr ("lock with std::mutex during registration,
/// lock-free read-only at runtime"). The registry holds a strong ref so the tokenizer outlives
/// every borrower.
class TokenizerRegistry {
public:
    /// Load `tokenizer.json` at `path` and register it under `key` (e.g.
    /// "bge-m3"). Idempotent for the same key: a second call with an already
    /// loaded key returns Ok without reloading. Returns the load error otherwise.
    static Status LoadAndRegister(const std::string& key, const std::string& path);

    /// Borrow the tokenizer registered under `key`, or nullptr if absent.
    static std::shared_ptr<HfTokenizer> Get(const std::string& key);

    /// Test-only: drop all registrations.
    static void ResetForTest();

private:
    TokenizerRegistry() = default;
};

}  // namespace cortrix::ml
