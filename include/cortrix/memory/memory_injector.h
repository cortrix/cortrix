#pragma once
#include <string>
#include <vector>
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_config.h"

namespace cortrix {

struct InjectedContext {
    std::string context_text;          // assembled context text
    int turn_count = 0;               // actual injected turn count
    int token_count_approx = 0;       // approximate token count
};

class MemoryInjector {
public:
    /// @param memory_store: MemoryStore (per-namespace)
    /// @param config: MemoryConfig
    MemoryInjector(MemoryStore& memory_store,
                   const MemoryConfig& config);

    /// Get recent N turns of conversation context for a session
    ///
    /// Flow:
    /// 1. MemoryStore.InteractionGetRecent(session_id, inject_recent_turns * 2)
    /// 2. Sort by time ascending
    /// 3. Assemble as conversation format:
    ///    "User: {query}\nAssistant: {response}\n\n..."
    /// 4. Truncate to inject_max_tokens (approximate)
    ///
    /// @param session_id: session ID
    /// @return InjectedContext
    InjectedContext GetRecentContext(const std::string& session_id);

private:
    /// Approximate token count (English: space-split, Chinese: per-character)
    static int ApproxTokenCount(const std::string& text);

    MemoryStore& memory_store_;
    MemoryConfig config_;
};

}  // namespace cortrix
