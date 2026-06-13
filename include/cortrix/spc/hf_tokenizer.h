#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include "cortrix/common/status.h"

namespace cortrix {

/// Tokenizer compatible with HuggingFace tokenizer.json format.
/// Supports both BPE (GPT-2/XLM-R) and Unigram (SentencePiece/bge-m3) models.
class HfTokenizer {
public:
    struct Encoded {
        std::vector<int64_t> input_ids;
        std::vector<int64_t> attention_mask;
    };

    /// Load tokenizer from HuggingFace tokenizer.json file
    Status Load(const std::string& tokenizer_json_path);

    /// Encode text into token IDs with attention mask
    /// Adds [CLS] and [SEP] tokens, pads/truncates to max_length
    Status Encode(const std::string& text, int max_length, Encoded* output) const;

    bool loaded() const { return loaded_; }

private:
    bool loaded_ = false;

    enum class ModelType { kBPE, kUnigram };
    ModelType model_type_ = ModelType::kBPE;

    // --- Common vocab ---
    // For BPE: token_string -> token_id
    // For Unigram: token_string -> token_id (built from vocab array)
    std::unordered_map<std::string, int> vocab_;

    // --- Unigram-specific ---
    // Token scores (log probabilities) for Viterbi decoding
    std::unordered_map<std::string, float> vocab_scores_;

    // --- BPE-specific ---
    // BPE merge ranks: (token_a, token_b) -> priority rank (lower = merge first)
    struct PairHash {
        size_t operator()(const std::pair<std::string, std::string>& p) const {
            auto h1 = std::hash<std::string>{}(p.first);
            auto h2 = std::hash<std::string>{}(p.second);
            return h1 ^ (h2 * 2654435761u);
        }
    };
    std::unordered_map<std::pair<std::string, std::string>, int, PairHash> merge_ranks_;

    // Byte-to-Unicode mapping for BPE byte-level encoding
    std::string byte_encoder_[256];

    // --- Special token IDs ---
    int cls_id_ = 0;    // <s>
    int sep_id_ = 2;    // </s>
    int pad_id_ = 1;    // <pad>
    int unk_id_ = 3;    // <unk>

    // --- Metaspace pre-tokenizer config ---
    std::string replacement_ = "\xe2\x96\x81";  // ▁ (U+2581) default for SentencePiece
    bool add_prefix_space_ = true;

    // --- BPE methods ---
    void InitByteEncoder();
    std::string ByteEncode(const std::string& text) const;
    std::vector<std::string> BpePreTokenize(const std::string& text) const;
    std::vector<std::string> BpeWord(const std::string& word) const;

    // --- Unigram methods ---
    /// Unigram pre-tokenize: apply Metaspace replacement
    std::string UnigramPreProcess(const std::string& text) const;
    /// Viterbi decode: find best segmentation for processed text
    std::vector<int> UnigramEncode(const std::string& text) const;

    // --- UTF-8 utilities ---
    static std::vector<std::string> Utf8Chars(const std::string& s);
    static std::string Char32ToUtf8(char32_t cp);
    /// Get byte length of a UTF-8 character from its first byte
    static int Utf8CharLen(uint8_t first_byte);
};

}  // namespace cortrix
