#pragma once
#include <string>

// MEM02 prompt templates (D10 decision: F03 framework reuse + MEM02 specialization).
// V1 ships a Chinese + English extraction prompt and a Chinese + English
// contradiction-judgment prompt. Multi-language (JA/KO/ES/FR/DE) is Phase 2
// (TD-MEM02-MULTI-LANG-PROMPT). The templates are plain text with `{placeholder}`
// tokens; RenderPrompt() does the single-pass substitution (no external template
// engine — keeps the kernel dependency-free).
namespace cortrix::memory {

/// Placeholder token substituted with the multi-turn interaction window text
/// (see MemoryExtractor::BuildWindowText).
inline constexpr const char* kPlaceholderInteractionWindow = "{interaction_window}";

/// Placeholders substituted in the contradiction-judgment prompt.
inline constexpr const char* kPlaceholderNewFact = "{new_fact}";
inline constexpr const char* kPlaceholderOldFact = "{old_fact}";

// --- Extraction prompts (D10) ---

/// Chinese extraction prompt. The model returns a JSON array of
/// {type, content, confidence} objects (no surrounding prose). `type` is one of
/// fact/preference/event; an unrecognized value is coerced to `event` in the
/// code-layer fallback (D4), NOT in the prompt.
extern const char* const kMemoryExtractPromptZh;

/// English extraction prompt (same contract as the Chinese variant).
extern const char* const kMemoryExtractPromptEn;

// --- Contradiction-judgment prompts (D5) ---

/// Chinese contradiction-judgment prompt. The model returns a single JSON object
/// {is_contradiction: bool, reason: string, confidence: number}.
extern const char* const kContradictionDetectPromptZh;

/// English contradiction-judgment prompt (same contract).
extern const char* const kContradictionDetectPromptEn;

/// Language selector for the V1 two-language prompt set (D10). `kAuto` picks ZH
/// when the input contains CJK characters, otherwise EN.
enum class PromptLang {
    kAuto = 0,
    kZh,
    kEn,
};

/// Heuristic: true iff `text` contains at least one CJK Unified Ideograph
/// (U+4E00..U+9FFF) in its UTF-8 bytes. Used by kAuto to pick the ZH prompt.
bool ContainsCjk(const std::string& text);

/// Return the extraction prompt template for `lang` (kAuto resolved via the CJK
/// heuristic against `sample_text`).
const char* ExtractPromptTemplate(PromptLang lang, const std::string& sample_text);

/// Return the contradiction-judgment prompt template for `lang` (kAuto resolved
/// via the CJK heuristic against `sample_text`).
const char* ContradictionPromptTemplate(PromptLang lang, const std::string& sample_text);

/// Single-pass substitution of every occurrence of `token` in `tmpl` with
/// `value`. Pure string op (no regex); used to fill the prompt placeholders.
std::string RenderPrompt(const std::string& tmpl,
                         const std::string& token,
                         const std::string& value);

}  // namespace cortrix::memory
