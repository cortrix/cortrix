#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cortrix/common/status.h"

namespace cortrix::query {

/// WordPieceTokenizer — the BERT/DistilBERT WordPiece tokenizer the F39/F37
/// query-complexity classifier (OnnxComplexityBackend) feeds. It reproduces the
/// HuggingFace `distilbert-base-uncased` pipeline byte-for-byte so the ids match
/// the model the classifier was trained/exported with:
///
///   1. BertNormalizer  — clean_text (control-char strip + whitespace fold),
///                        handle_chinese_chars (pad each CJK ideograph with
///                        spaces), lowercase, strip_accents (NFD → drop the
///                        Unicode Mn combining marks). All four are ON for this
///                        checkpoint (tokenizer.json normalizer).
///   2. BertPreTokenizer — split on whitespace, then split off every punctuation
///                        char as its own token.
///   3. WordPiece        — greedy longest-match-first over the vocab, "##"
///                        continuation prefix, [UNK] for an unmatched word (or a
///                        word longer than max_input_chars_per_word=100).
///   4. TemplateProcessing — wrap with [CLS] … [SEP], truncate the content to
///                        max_length-2, build the attention_mask (no padding
///                        here — the caller pads a batch if it ever needs to).
///
/// Vocab source: the `vocab.txt` next to model.onnx (one token per line, id =
/// line number, 0-based). tokenizer.json's `model.vocab` is the identical map; we
/// load vocab.txt because it is the simplest stable form and matches the HF
/// `distilbert-base-uncased` contract documented in the model README.
///
/// This is deliberately a *separate* type from the BPE/Unigram `cortrix::HfTokenizer`
/// (which serves bge-m3, a SentencePiece Unigram model): the two share no code
/// path, and keeping WordPiece standalone avoids growing that class a third model
/// family. The Encoded shape mirrors HfTokenizer::Encoded so the ONNX-feed code is
/// familiar.
class WordPieceTokenizer {
public:
    struct Encoded {
        std::vector<int64_t> input_ids;       ///< [CLS] … [SEP], length ≤ max_length
        std::vector<int64_t> attention_mask;  ///< all 1s (single sequence, no padding)
    };

    /// Load the vocab from a `vocab.txt` (one token per line; id = 0-based line
    /// index). Resolves the [CLS]/[SEP]/[UNK]/[PAD] ids from the table by their
    /// canonical surface strings. Returns NotFound when the file is missing and
    /// DataLoss when a required special token is absent from the vocab.
    Status LoadVocab(const std::string& vocab_txt_path);

    /// Encode `text` into [CLS] + WordPiece(content)[:max_length-2] + [SEP] with a
    /// matching all-ones attention_mask. `max_length` includes the two special
    /// tokens (so at most max_length-2 content tokens survive truncation). Total:
    /// never throws; an empty/whitespace-only input yields just [CLS] [SEP].
    Status Encode(const std::string& text, int max_length, Encoded* output) const;

    bool loaded() const { return loaded_; }
    size_t vocab_size() const { return vocab_.size(); }

    int cls_id() const { return cls_id_; }
    int sep_id() const { return sep_id_; }
    int unk_id() const { return unk_id_; }
    int pad_id() const { return pad_id_; }

    // --- Exposed for unit tests (the pure string stages, no special tokens). ---

    /// BertNormalizer + BertPreTokenizer: returns the normalized, punctuation- and
    /// CJK-split "words" that WordPiece then runs over. Pure, static-like helper.
    std::vector<std::string> BasicTokenize(const std::string& text) const;

    /// Greedy longest-match-first WordPiece over a single pre-token `word`
    /// (already normalized). Returns the subword ids (one [UNK] id if no prefix
    /// matches or the word is over max_input_chars_per_word).
    std::vector<int64_t> WordPieceEncode(const std::string& word) const;

private:
    bool loaded_ = false;

    std::unordered_map<std::string, int64_t> vocab_;

    int cls_id_ = 101;
    int sep_id_ = 102;
    int unk_id_ = 100;
    int pad_id_ = 0;

    // HF `distilbert-base-uncased` WordPiece config (tokenizer.json model.*).
    static constexpr int kMaxInputCharsPerWord = 100;

    // --- normalizer + pre-tokenizer (operate on UTF-8 → returns UTF-8) ---

    /// BertNormalizer clean_text: drop control chars + \0, fold all whitespace
    /// (incl. \t\n\r and Unicode space separators) to a single ' '.
    static std::string CleanText(const std::string& text);

    /// handle_chinese_chars: surround every CJK ideograph with ' ' so it becomes
    /// its own pre-token (BERT tokenizes CJK per-character).
    static std::string PadChineseChars(const std::string& text);

    /// strip_accents: NFD-decompose, then drop every Unicode Mn (combining mark)
    /// code point. Applied after lowercasing for this checkpoint.
    static std::string StripAccents(const std::string& text);

    /// ASCII-fast lowercase that also lowercases the common Latin-1 range; full
    /// Unicode case-folding is unnecessary because strip_accents already collapses
    /// the accented Latin letters this model sees to their base ASCII form.
    static std::string ToLower(const std::string& text);

    // --- UTF-8 / Unicode helpers ---
    static bool IsWhitespace(char32_t cp);
    static bool IsControl(char32_t cp);
    static bool IsPunctuation(char32_t cp);
    static bool IsChineseChar(char32_t cp);
    static bool IsCombiningMark(char32_t cp);

    /// Decode one UTF-8 code point at byte offset `i`; advances `i` past it.
    static char32_t NextCodePoint(const std::string& s, size_t& i);
    /// Append `cp` to `out` as UTF-8.
    static void AppendUtf8(char32_t cp, std::string& out);
};

}  // namespace cortrix::query
