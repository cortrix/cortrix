#include "cortrix/memory/prompt_templates.h"

#include <cstdint>

namespace cortrix::memory {

// --- Extraction prompts — few-shot, JSON-array output contract ---
//
// The *Zh / *En pairs below are functional engine data (multilingual content
// support): ExtractPromptTemplate / ContradictionPromptTemplate pick the Chinese
// variant for Chinese-language input (CJK heuristic) and the English variant
// otherwise. The Chinese prompt bodies are therefore intentionally preserved (NOT
// stray translatable strings) — translating a *Zh body would collapse it into its
// *En twin and break language routing.

const char* const kMemoryExtractPromptZh = R"(你是 Cortrix 记忆提取助手。从用户对话中提取关键的长期记忆。

分类标准:
- fact: 客观、长期事实（例:"用户是医生"、"用户在上海"、"用户对青霉素过敏"）
- preference: 个人偏好（例:"用户喜欢 Python"、"偏好深色主题"）
- event: 事件或动态（例:"用户昨天问了天气"、"上周开了会"、"下个月开始新工作"）

少量样例:
对话: "我刚搬到上海，下个月开始新工作"
输出: [
  {"type": "fact", "content": "用户在上海", "confidence": 0.95},
  {"type": "event", "content": "用户下个月开始新工作", "confidence": 0.9}
]

对话: "我对青霉素过敏"
输出: [
  {"type": "fact", "content": "用户对青霉素过敏", "confidence": 0.99}
]

对话:
{interaction_window}

只输出 JSON 数组（无其他文字）:
)";

const char* const kMemoryExtractPromptEn = R"(You are the Cortrix memory extraction assistant. Extract key long-term memories from the user conversation.

Classification:
- fact: objective, long-term facts (e.g. "user is a doctor", "user is in Shanghai", "user is allergic to penicillin")
- preference: personal preferences (e.g. "user likes Python", "prefers dark theme")
- event: events or dynamics (e.g. "user asked about the weather yesterday", "had a meeting last week", "starts a new job next month")

Few-shot examples:
Conversation: "I just moved to Shanghai and start a new job next month"
Output: [
  {"type": "fact", "content": "user is in Shanghai", "confidence": 0.95},
  {"type": "event", "content": "user starts a new job next month", "confidence": 0.9}
]

Conversation: "I'm allergic to penicillin"
Output: [
  {"type": "fact", "content": "user is allergic to penicillin", "confidence": 0.99}
]

Conversation:
{interaction_window}

Output ONLY the JSON array (no other text):
)";

// --- Contradiction-judgment prompts — single JSON object output ---

const char* const kContradictionDetectPromptZh = R"(判断新事实和旧事实是否矛盾。

矛盾标准:
- 同一主体在同一维度的属性冲突（地理位置、时间、所属关系）
- 不矛盾: 互补、独立维度、临时事件 vs 长期事实

新事实: {new_fact}
旧事实: {old_fact}

只输出 JSON 对象:
{"is_contradiction": true|false, "reason": "...", "confidence": 0.0-1.0}
)";

const char* const kContradictionDetectPromptEn = R"(Determine whether the new fact contradicts the old fact.

Contradiction criteria:
- the same subject conflicts on the same dimension (location, time, affiliation)
- NOT a contradiction: complementary facts, independent dimensions, a temporary event vs a long-term fact

New fact: {new_fact}
Old fact: {old_fact}

Output ONLY the JSON object:
{"is_contradiction": true|false, "reason": "...", "confidence": 0.0-1.0}
)";

bool ContainsCjk(const std::string& text) {
    // Minimal UTF-8 decode looking only for the CJK Unified Ideographs block
    // (U+4E00..U+9FFF), which is encoded as 3-byte sequences 0xE4..0xE9 lead bytes
    // with the appropriate continuation. We scan for any 3-byte sequence whose
    // decoded code point falls in that range — enough to pick ZH vs EN.
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        const auto c = static_cast<uint8_t>(text[i]);
        if (c < 0x80) {
            i += 1;
        } else if ((c >> 5) == 0x6) {        // 110xxxxx — 2-byte
            i += 2;
        } else if ((c >> 4) == 0xE) {        // 1110xxxx — 3-byte
            if (i + 2 < n) {
                const auto c1 = static_cast<uint8_t>(text[i + 1]);
                const auto c2 = static_cast<uint8_t>(text[i + 2]);
                const uint32_t cp = ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
                if (cp >= 0x4E00u && cp <= 0x9FFFu) return true;
            }
            i += 3;
        } else if ((c >> 3) == 0x1E) {       // 11110xxx — 4-byte
            i += 4;
        } else {
            i += 1;                          // malformed lead byte; skip one
        }
    }
    return false;
}

const char* ExtractPromptTemplate(PromptLang lang, const std::string& sample_text) {
    switch (lang) {
        case PromptLang::kZh: return kMemoryExtractPromptZh;
        case PromptLang::kEn: return kMemoryExtractPromptEn;
        case PromptLang::kAuto:
        default:
            return ContainsCjk(sample_text) ? kMemoryExtractPromptZh
                                            : kMemoryExtractPromptEn;
    }
}

const char* ContradictionPromptTemplate(PromptLang lang, const std::string& sample_text) {
    switch (lang) {
        case PromptLang::kZh: return kContradictionDetectPromptZh;
        case PromptLang::kEn: return kContradictionDetectPromptEn;
        case PromptLang::kAuto:
        default:
            return ContainsCjk(sample_text) ? kContradictionDetectPromptZh
                                            : kContradictionDetectPromptEn;
    }
}

std::string RenderPrompt(const std::string& tmpl,
                         const std::string& token,
                         const std::string& value) {
    if (token.empty()) return tmpl;
    std::string out;
    out.reserve(tmpl.size() + value.size());
    size_t pos = 0;
    for (;;) {
        const size_t hit = tmpl.find(token, pos);
        if (hit == std::string::npos) {
            out.append(tmpl, pos, std::string::npos);
            break;
        }
        out.append(tmpl, pos, hit - pos);
        out.append(value);
        pos = hit + token.size();
    }
    return out;
}

}  // namespace cortrix::memory
