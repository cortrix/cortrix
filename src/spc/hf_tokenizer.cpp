#include <cstdint>
#include "cortrix/spc/hf_tokenizer.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <climits>
#include <cmath>
#include <spdlog/spdlog.h>

namespace cortrix {

// --- UTF-8 utilities ---

std::string HfTokenizer::Char32ToUtf8(char32_t cp) {
    std::string r;
    if (cp < 0x80) {
        r += static_cast<char>(cp);
    } else if (cp < 0x800) {
        r += static_cast<char>(0xC0 | (cp >> 6));
        r += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        r += static_cast<char>(0xE0 | (cp >> 12));
        r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        r += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        r += static_cast<char>(0xF0 | (cp >> 18));
        r += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        r += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return r;
}

std::vector<std::string> HfTokenizer::Utf8Chars(const std::string& s) {
    std::vector<std::string> chars;
    size_t i = 0;
    while (i < s.size()) {
        int len = Utf8CharLen(static_cast<uint8_t>(s[i]));
        if (i + len > s.size()) len = 1;  // Safety
        chars.push_back(s.substr(i, len));
        i += len;
    }
    return chars;
}

int HfTokenizer::Utf8CharLen(uint8_t first_byte) {
    if (first_byte < 0x80) return 1;
    if (first_byte < 0xC0) return 1;  // continuation byte (shouldn't start here)
    if (first_byte < 0xE0) return 2;
    if (first_byte < 0xF0) return 3;
    return 4;
}

// ================================================================
// BPE methods (kept from original implementation)
// ================================================================

void HfTokenizer::InitByteEncoder() {
    // GPT-2 / XLM-RoBERTa byte-to-unicode mapping
    std::vector<int> bs;
    for (int i = 33; i <= 126; i++) bs.push_back(i);
    for (int i = 161; i <= 172; i++) bs.push_back(i);
    for (int i = 174; i <= 255; i++) bs.push_back(i);

    std::vector<int> cs(bs.begin(), bs.end());

    int n = 0;
    for (int b = 0; b < 256; b++) {
        if (std::find(bs.begin(), bs.end(), b) == bs.end()) {
            bs.push_back(b);
            cs.push_back(256 + n);
            n++;
        }
    }

    for (size_t i = 0; i < bs.size(); i++) {
        byte_encoder_[static_cast<uint8_t>(bs[i])] = Char32ToUtf8(static_cast<char32_t>(cs[i]));
    }
}

std::string HfTokenizer::ByteEncode(const std::string& text) const {
    std::string result;
    for (uint8_t c : text) {
        result += byte_encoder_[c];
    }
    return result;
}

std::vector<std::string> HfTokenizer::BpePreTokenize(const std::string& text) const {
    std::string t = text;
    if (!t.empty() && t[0] != ' ') {
        t = " " + t;
    }

    std::vector<std::string> words;
    size_t pos = 0;

    while (pos < t.size()) {
        size_t word_start = pos;
        while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t' ||
                                   t[pos] == '\n' || t[pos] == '\r')) {
            pos++;
        }
        while (pos < t.size() && t[pos] != ' ' && t[pos] != '\t' &&
               t[pos] != '\n' && t[pos] != '\r') {
            pos++;
        }
        if (pos > word_start) {
            std::string word = t.substr(word_start, pos - word_start);
            words.push_back(ByteEncode(word));
        }
    }
    return words;
}

std::vector<std::string> HfTokenizer::BpeWord(const std::string& word) const {
    auto chars = Utf8Chars(word);
    if (chars.size() <= 1) return chars;

    while (true) {
        int best_rank = INT_MAX;
        std::pair<std::string, std::string> best_pair;
        bool found = false;

        for (size_t i = 0; i + 1 < chars.size(); i++) {
            auto pair = std::make_pair(chars[i], chars[i + 1]);
            auto it = merge_ranks_.find(pair);
            if (it != merge_ranks_.end() && it->second < best_rank) {
                best_rank = it->second;
                best_pair = pair;
                found = true;
            }
        }

        if (!found) break;

        std::string merged = best_pair.first + best_pair.second;
        std::vector<std::string> new_chars;
        size_t i = 0;
        while (i < chars.size()) {
            if (i + 1 < chars.size() &&
                chars[i] == best_pair.first && chars[i + 1] == best_pair.second) {
                new_chars.push_back(merged);
                i += 2;
            } else {
                new_chars.push_back(chars[i]);
                i++;
            }
        }
        chars = std::move(new_chars);
        if (chars.size() <= 1) break;
    }
    return chars;
}

// ================================================================
// Unigram methods (SentencePiece Viterbi)
// ================================================================

std::string HfTokenizer::UnigramPreProcess(const std::string& text) const {
    // Metaspace pre-tokenizer: replace spaces with ▁ and optionally add prefix
    std::string result;
    if (add_prefix_space_) {
        result = replacement_;
    }
    for (char c : text) {
        if (c == ' ') {
            result += replacement_;
        } else {
            result += c;
        }
    }
    return result;
}

std::vector<int> HfTokenizer::UnigramEncode(const std::string& processed) const {
    // Viterbi algorithm for Unigram tokenization
    // dp[i] = {best_score, best_token_end_pos} for position i
    int n = static_cast<int>(processed.size());
    if (n == 0) return {};

    const float kNegInf = -1e30f;

    // best_score[i] = best cumulative log-prob to reach position i
    // best_prev[i] = the start position of the token that ends at i
    std::vector<float> best_score(n + 1, kNegInf);
    std::vector<int> best_prev(n + 1, -1);
    best_score[0] = 0.0f;

    // For each position, try all possible tokens starting here
    for (int i = 0; i < n; i++) {
        if (best_score[i] == kNegInf) continue;

        // Try substrings of increasing length from position i
        // Limit max token length to avoid O(n^2) in extreme cases
        int max_token_len = std::min(n - i, 128);  // Most tokens are < 128 bytes

        for (int len = 1; len <= max_token_len; len++) {
            // Only try substrings that end on UTF-8 character boundaries
            int end = i + len;
            if (end <= n) {
                // Check if this ends on a valid UTF-8 boundary
                if (end < n) {
                    uint8_t next_byte = static_cast<uint8_t>(processed[end]);
                    // If next byte is a continuation byte (10xxxxxx), we're mid-character
                    if ((next_byte & 0xC0) == 0x80) continue;
                }

                std::string token = processed.substr(i, len);
                auto it = vocab_scores_.find(token);
                if (it != vocab_scores_.end()) {
                    float new_score = best_score[i] + it->second;
                    if (new_score > best_score[end]) {
                        best_score[end] = new_score;
                        best_prev[end] = i;
                    }
                }
            }
        }

        // Fallback: if no token matches from this position, use single byte/char as UNK
        int char_len = Utf8CharLen(static_cast<uint8_t>(processed[i]));
        if (i + char_len <= n) {
            float unk_score = best_score[i] + (-100.0f);  // Heavy penalty for UNK
            if (unk_score > best_score[i + char_len]) {
                best_score[i + char_len] = unk_score;
                best_prev[i + char_len] = i;
            }
        }
    }

    // Backtrack to get token sequence
    std::vector<std::string> tokens;
    int pos = n;
    while (pos > 0) {
        int prev = best_prev[pos];
        if (prev < 0) {
            // Should not happen if UNK fallback works
            break;
        }
        tokens.push_back(processed.substr(prev, pos - prev));
        pos = prev;
    }
    std::reverse(tokens.begin(), tokens.end());

    // Convert to token IDs
    std::vector<int> ids;
    ids.reserve(tokens.size());
    for (const auto& tok : tokens) {
        auto it = vocab_.find(tok);
        if (it != vocab_.end()) {
            ids.push_back(it->second);
        } else {
            ids.push_back(unk_id_);
        }
    }
    return ids;
}

// ================================================================
// Loading
// ================================================================

Status HfTokenizer::Load(const std::string& tokenizer_json_path) {
    std::ifstream f(tokenizer_json_path);
    if (!f.is_open()) {
        return Status::NotFound("tokenizer file not found: " + tokenizer_json_path);
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::exception& e) {
        return Status::Internal("failed to parse tokenizer.json: " + std::string(e.what()));
    }

    if (!j.contains("model")) {
        return Status::Internal("tokenizer.json missing 'model' section");
    }

    auto& model = j["model"];
    std::string model_type = model.value("type", "BPE");

    if (model_type == "Unigram") {
        // ---- Unigram model ----
        model_type_ = ModelType::kUnigram;

        if (!model.contains("vocab") || !model["vocab"].is_array()) {
            return Status::Internal("Unigram tokenizer.json missing model.vocab array");
        }

        // Vocab is array of [token_string, score] pairs
        int id = 0;
        for (const auto& entry : model["vocab"]) {
            if (!entry.is_array() || entry.size() < 2) continue;
            std::string token = entry[0].get<std::string>();
            float score = entry[1].get<float>();
            vocab_[token] = id;
            vocab_scores_[token] = score;
            id++;
        }

        spdlog::info("Loaded Unigram tokenizer: {} tokens", vocab_.size());

    } else {
        // ---- BPE model ----
        model_type_ = ModelType::kBPE;
        InitByteEncoder();

        if (!model.contains("vocab") || !model["vocab"].is_object()) {
            return Status::Internal("BPE tokenizer.json missing model.vocab object");
        }

        for (auto& [token, token_id] : model["vocab"].items()) {
            vocab_[token] = token_id.get<int>();
        }

        if (model.contains("merges")) {
            int rank = 0;
            for (const auto& merge : model["merges"]) {
                std::string m = merge.get<std::string>();
                auto space_pos = m.find(' ');
                if (space_pos != std::string::npos) {
                    std::string a = m.substr(0, space_pos);
                    std::string b = m.substr(space_pos + 1);
                    merge_ranks_[std::make_pair(a, b)] = rank++;
                }
            }
        }

        spdlog::info("Loaded BPE tokenizer: {} tokens, {} merges",
                      vocab_.size(), merge_ranks_.size());
    }

    // Load pre-tokenizer config
    if (j.contains("pre_tokenizer")) {
        auto& pt = j["pre_tokenizer"];
        std::string pt_type = pt.value("type", "");
        if (pt_type == "Metaspace") {
            replacement_ = pt.value("replacement", std::string("\xe2\x96\x81"));
            add_prefix_space_ = pt.value("add_prefix_space", true);
        }
    }

    // Load special token IDs from added_tokens
    if (j.contains("added_tokens")) {
        for (const auto& tok : j["added_tokens"]) {
            std::string content = tok.value("content", "");
            int id = tok.value("id", -1);
            if (content == "<s>") cls_id_ = id;
            else if (content == "</s>") sep_id_ = id;
            else if (content == "<pad>") pad_id_ = id;
            else if (content == "<unk>") unk_id_ = id;
        }
    }

    loaded_ = true;
    return Status::Ok();
}

// ================================================================
// Encoding
// ================================================================

Status HfTokenizer::Encode(const std::string& text, int max_length, Encoded* output) const {
    return EncodeInternal(text, max_length, /*pad_to_max_length=*/true, output);
}

Status HfTokenizer::EncodeNoPad(const std::string& text,
                                int max_length,
                                Encoded* output) const {
    return EncodeInternal(text, max_length, /*pad_to_max_length=*/false, output);
}

Status HfTokenizer::EncodeInternal(const std::string& text,
                                   int max_length,
                                   bool pad_to_max_length,
                                   Encoded* output) const {
    if (!output) return Status::InvalidArgument("null output pointer");
    if (!loaded_) return Status::Internal("tokenizer not loaded");
    if (max_length < 2) return Status::InvalidArgument("max_length must be >= 2");

    std::vector<int64_t> ids;
    ids.push_back(cls_id_);  // <s>

    if (model_type_ == ModelType::kUnigram) {
        // Unigram encoding
        std::string processed = UnigramPreProcess(text);
        auto token_ids = UnigramEncode(processed);
        for (int tid : token_ids) {
            ids.push_back(static_cast<int64_t>(tid));
        }
    } else {
        // BPE encoding
        auto words = BpePreTokenize(text);
        for (const auto& word : words) {
            auto bpe_tokens = BpeWord(word);
            for (const auto& tok : bpe_tokens) {
                auto it = vocab_.find(tok);
                if (it != vocab_.end()) {
                    ids.push_back(it->second);
                } else {
                    ids.push_back(unk_id_);
                }
            }
        }
    }

    ids.push_back(sep_id_);  // </s>

    // Truncate if needed (keep CLS at start and SEP at end)
    if (static_cast<int>(ids.size()) > max_length) {
        ids.resize(max_length - 1);
        ids.push_back(sep_id_);
    }

    // Create attention mask
    int real_len = static_cast<int>(ids.size());
    output->input_ids = std::move(ids);
    output->attention_mask.assign(real_len, 1);

    if (pad_to_max_length) {
        while (static_cast<int>(output->input_ids.size()) < max_length) {
            output->input_ids.push_back(pad_id_);
            output->attention_mask.push_back(0);
        }
    }

    return Status::Ok();
}

}  // namespace cortrix
