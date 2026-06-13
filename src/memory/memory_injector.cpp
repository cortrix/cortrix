#include "cortrix/memory/memory_injector.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace cortrix {

MemoryInjector::MemoryInjector(MemoryStore& memory_store,
                               const MemoryConfig& config)
    : memory_store_(memory_store)
    , config_(config) {}

InjectedContext MemoryInjector::GetRecentContext(const std::string& session_id) {
    InjectedContext ctx;

    // 1. Get recent interactions (N turns * 2 = user + assistant per turn)
    int fetch_count = config_.inject_recent_turns * 2;
    std::vector<InteractionLog> interactions;
    auto s = memory_store_.InteractionGetRecent(session_id, fetch_count, interactions);
    if (!s.ok() || interactions.empty()) {
        return ctx;
    }

    // 2. Reverse to time-ascending order (oldest first)
    std::reverse(interactions.begin(), interactions.end());

    // 3. Pair user/assistant and assemble conversation format
    std::string result;
    int token_count = 0;
    int turn_count = 0;

    size_t i = 0;
    while (i < interactions.size()) {
        // Find a user message
        if (interactions[i].role != "user") {
            ++i;
            continue;
        }

        // Look for the next assistant message
        std::string user_content = interactions[i].content;
        std::string assistant_content;

        if (i + 1 < interactions.size() && interactions[i + 1].role == "assistant") {
            assistant_content = interactions[i + 1].content;
            i += 2;
        } else {
            // No paired assistant response, skip
            ++i;
            continue;
        }

        std::string turn_text = "User: " + user_content + "\n"
                                "Assistant: " + assistant_content + "\n\n";

        int turn_tokens = ApproxTokenCount(turn_text);

        if (token_count + turn_tokens > config_.inject_max_tokens) {
            break;  // token limit reached
        }

        result += turn_text;
        token_count += turn_tokens;
        ++turn_count;
    }

    ctx.context_text = result;
    ctx.turn_count = turn_count;
    ctx.token_count_approx = token_count;
    return ctx;
}

int MemoryInjector::ApproxTokenCount(const std::string& text) {
    if (text.empty()) return 0;

    int count = 0;
    bool in_space = true;

    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    size_t len = text.size();

    for (size_t i = 0; i < len; ) {
        unsigned char c = bytes[i];

        if (c < 0x80) {
            // ASCII
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                in_space = true;
            } else {
                if (in_space) {
                    ++count;
                    in_space = false;
                }
            }
            ++i;
        } else if (c < 0xC0) {
            // continuation byte (shouldn't happen at start)
            ++i;
        } else if (c < 0xE0) {
            // 2-byte UTF-8
            ++count;  // count each multi-byte char as 1 token
            i += 2;
            in_space = true;
        } else if (c < 0xF0) {
            // 3-byte UTF-8 (CJK characters fall here)
            // Design spec: CJK characters count as 2 tokens each
            count += 2;
            i += 3;
            in_space = true;
        } else {
            // 4-byte UTF-8
            ++count;
            i += 4;
            in_space = true;
        }
    }

    return count;
}

}  // namespace cortrix
