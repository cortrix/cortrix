#include "cortrix/query/wordpiece_tokenizer.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cortrix::query {

namespace {

// NFD decomposition for the precomposed accented Latin letters this model sees
// (Latin-1 Supplement U+00C0–U+00FF + a slice of Latin Extended-A). Each entry
// maps the precomposed code point to its base letter; the combining mark it
// decomposes into is then dropped by StripAccents (matching HF BertNormalizer
// NFD → drop Mn). We table the base letter directly rather than emit a real
// combining mark + re-scan, which is equivalent for these single-mark forms and
// avoids shipping a full Unicode NFD database. Code points outside this table
// (e.g. CJK, emoji) carry no combining accent and pass through unchanged.
char32_t StripLatinAccent(char32_t cp) {
    switch (cp) {
        // U+00C0–U+00C5 À Á Â Ã Ä Å
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: return 'A';
        case 0x00C6: return cp;  // Æ — not a base+mark; HF keeps it (no NFD split)
        case 0x00C7: return 'C';                                   // Ç
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return 'E';  // È É Ê Ë
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return 'I';  // Ì Í Î Ï
        case 0x00D0: return cp;                                    // Ð
        case 0x00D1: return 'N';                                   // Ñ
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: return 'O';  // Ò Ó Ô Õ Ö
        case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return 'U';  // Ù Ú Û Ü
        case 0x00DD: return 'Y';                                   // Ý
        // U+00E0–U+00E5 à á â ã ä å
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: return 'a';
        case 0x00E6: return cp;  // æ
        case 0x00E7: return 'c';                                   // ç
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return 'e';  // è é ê ë
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return 'i';  // ì í î ï
        case 0x00F0: return cp;                                    // ð
        case 0x00F1: return 'n';                                   // ñ
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: return 'o';  // ò ó ô õ ö
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return 'u';  // ù ú û ü
        case 0x00FD: case 0x00FF: return 'y';                      // ý ÿ
        default: break;
    }
    // Latin Extended-A (U+0100–U+017F): every even/odd pair is an accented
    // upper/lower letter that NFD-decomposes to a base ASCII letter. We map the
    // handful most common in European text; the rest fall through unchanged
    // (they would [UNK] anyway, which still matches HF for vocab-absent forms).
    switch (cp) {
        case 0x0100: case 0x0102: case 0x0104: return 'A';
        case 0x0101: case 0x0103: case 0x0105: return 'a';
        case 0x0106: case 0x0108: case 0x010A: case 0x010C: return 'C';
        case 0x0107: case 0x0109: case 0x010B: case 0x010D: return 'c';
        case 0x0112: case 0x0114: case 0x0116: case 0x0118: case 0x011A: return 'E';
        case 0x0113: case 0x0115: case 0x0117: case 0x0119: case 0x011B: return 'e';
        case 0x011E: case 0x0120: return 'G';
        case 0x011F: case 0x0121: return 'g';
        case 0x0128: case 0x012A: case 0x012C: case 0x012E: return 'I';
        case 0x0129: case 0x012B: case 0x012D: case 0x012F: return 'i';
        case 0x0141: return 'L';
        case 0x0142: return 'l';
        case 0x0143: case 0x0145: case 0x0147: return 'N';
        case 0x0144: case 0x0146: case 0x0148: return 'n';
        case 0x014C: case 0x014E: case 0x0150: return 'O';
        case 0x014D: case 0x014F: case 0x0151: return 'o';
        case 0x0154: case 0x0156: case 0x0158: return 'R';
        case 0x0155: case 0x0157: case 0x0159: return 'r';
        case 0x015A: case 0x015C: case 0x015E: case 0x0160: return 'S';
        case 0x015B: case 0x015D: case 0x015F: case 0x0161: return 's';
        case 0x0162: case 0x0164: return 'T';
        case 0x0163: case 0x0165: return 't';
        case 0x0168: case 0x016A: case 0x016C: case 0x016E: case 0x0170: case 0x0172: return 'U';
        case 0x0169: case 0x016B: case 0x016D: case 0x016F: case 0x0171: case 0x0173: return 'u';
        case 0x0179: case 0x017B: case 0x017D: return 'Z';
        case 0x017A: case 0x017C: case 0x017E: return 'z';
        default: return cp;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// UTF-8 / Unicode helpers
// ---------------------------------------------------------------------------

char32_t WordPieceTokenizer::NextCodePoint(const std::string& s, size_t& i) {
    const auto n = s.size();
    if (i >= n) return 0;
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        ++i;
        return c0;
    }
    int len = 0;
    char32_t cp = 0;
    if ((c0 & 0xE0) == 0xC0) { len = 2; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { len = 3; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { len = 4; cp = c0 & 0x07; }
    else { ++i; return 0xFFFD; }  // invalid lead byte
    if (i + len > n) { ++i; return 0xFFFD; }
    for (int k = 1; k < len; ++k) {
        const unsigned char ck = static_cast<unsigned char>(s[i + k]);
        if ((ck & 0xC0) != 0x80) { ++i; return 0xFFFD; }  // bad continuation
        cp = (cp << 6) | (ck & 0x3F);
    }
    i += len;
    return cp;
}

void WordPieceTokenizer::AppendUtf8(char32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool WordPieceTokenizer::IsWhitespace(char32_t cp) {
    // BERT _is_whitespace: ASCII ws + the Unicode space separators it folds.
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return true;
    switch (cp) {
        case 0x00A0: case 0x1680:
        case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
        case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
        case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
        case 0x3000:
            return true;
        default:
            return false;
    }
}

bool WordPieceTokenizer::IsControl(char32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\r') return false;  // treated as ws
    if (cp == 0) return true;
    // C0 (excl. the ws above) + DEL + C1 controls.
    if (cp < 0x20 || cp == 0x7F) return true;
    if (cp >= 0x80 && cp <= 0x9F) return true;
    // Format chars (Cf) BERT also strips: zero-width + BOM + bidi marks.
    switch (cp) {
        case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F:
        case 0x202A: case 0x202B: case 0x202C: case 0x202D: case 0x202E:
        case 0xFEFF:
            return true;
        default:
            return false;
    }
}

bool WordPieceTokenizer::IsPunctuation(char32_t cp) {
    // BERT _is_punctuation: all ASCII non-alphanumeric printable + every Unicode
    // P* category. We approximate the Unicode side with the ASCII ranges (exact)
    // plus the common general-punctuation blocks; this matches the model's vocab,
    // which has no non-ASCII punctuation tokens anyway.
    if ((cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64) ||
        (cp >= 91 && cp <= 96) || (cp >= 123 && cp <= 126)) {
        return true;
    }
    // General Punctuation (U+2000–U+206F, the non-space part) + CJK symbols.
    if (cp >= 0x2010 && cp <= 0x2027) return true;  // dashes, quotes, bullets …
    if (cp >= 0x2030 && cp <= 0x205E) return true;
    if (cp >= 0x3001 && cp <= 0x303F) return true;  // CJK punctuation
    if (cp >= 0xFF01 && cp <= 0xFF0F) return true;  // fullwidth ASCII punct
    if (cp >= 0xFF1A && cp <= 0xFF20) return true;
    return false;
}

bool WordPieceTokenizer::IsChineseChar(char32_t cp) {
    // BERT _is_chinese_char: the CJK Unified Ideograph blocks (not punctuation).
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) ||
           (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) ||
           (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x2F800 && cp <= 0x2FA1F);
}

bool WordPieceTokenizer::IsCombiningMark(char32_t cp) {
    // Unicode Mn (Nonspacing_Mark) ranges that NFD produces for Latin/Greek/etc.
    return (cp >= 0x0300 && cp <= 0x036F) ||  // Combining Diacritical Marks
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE20 && cp <= 0xFE2F);
}

// ---------------------------------------------------------------------------
// Normalizer stages
// ---------------------------------------------------------------------------

std::string WordPieceTokenizer::CleanText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        char32_t cp = NextCodePoint(text, i);
        if (cp == 0 || cp == 0xFFFD) continue;  // strip \0 + invalid bytes
        if (IsControl(cp)) continue;
        if (IsWhitespace(cp)) {
            out.push_back(' ');
        } else {
            AppendUtf8(cp, out);
        }
    }
    return out;
}

std::string WordPieceTokenizer::PadChineseChars(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    size_t i = 0;
    while (i < text.size()) {
        char32_t cp = NextCodePoint(text, i);
        if (IsChineseChar(cp)) {
            out.push_back(' ');
            AppendUtf8(cp, out);
            out.push_back(' ');
        } else {
            AppendUtf8(cp, out);
        }
    }
    return out;
}

std::string WordPieceTokenizer::ToLower(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        char32_t cp = NextCodePoint(text, i);
        if (cp >= 'A' && cp <= 'Z') {
            cp = cp - 'A' + 'a';
        } else if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) {
            // Latin-1 uppercase → lowercase (offset 0x20), excluding × (0x00D7).
            cp += 0x20;
        }
        AppendUtf8(cp, out);
    }
    return out;
}

std::string WordPieceTokenizer::StripAccents(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        char32_t cp = NextCodePoint(text, i);
        if (IsCombiningMark(cp)) continue;  // drop any already-decomposed mark
        cp = StripLatinAccent(cp);          // fold precomposed Latin → base letter
        AppendUtf8(cp, out);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Basic (normalizer + pre-tokenizer)
// ---------------------------------------------------------------------------

std::vector<std::string> WordPieceTokenizer::BasicTokenize(const std::string& text) const {
    // Normalizer: clean → handle CJK → lowercase → strip accents (this checkpoint
    // has all four on). Lowercase before strip_accents so the Latin-1 uppercase
    // letters fold first, matching HF's order (lowercase then NFD/Mn-drop).
    std::string norm = StripAccents(ToLower(PadChineseChars(CleanText(text))));

    // Pre-tokenizer: whitespace split, then break off each punctuation char.
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            words.push_back(cur);
            cur.clear();
        }
    };
    size_t i = 0;
    while (i < norm.size()) {
        const size_t start = i;
        char32_t cp = NextCodePoint(norm, i);
        if (cp == ' ') {
            flush();
        } else if (IsPunctuation(cp)) {
            flush();
            words.push_back(norm.substr(start, i - start));  // punctuation is its own token
        } else {
            cur.append(norm, start, i - start);
        }
    }
    flush();
    return words;
}

// ---------------------------------------------------------------------------
// WordPiece (greedy longest-match-first)
// ---------------------------------------------------------------------------

std::vector<int64_t> WordPieceTokenizer::WordPieceEncode(const std::string& word) const {
    std::vector<int64_t> ids;

    // Count chars (code points) to enforce max_input_chars_per_word.
    size_t char_count = 0;
    for (size_t j = 0; j < word.size();) {
        NextCodePoint(word, j);
        ++char_count;
    }
    if (char_count == 0) return ids;
    if (char_count > static_cast<size_t>(kMaxInputCharsPerWord)) {
        ids.push_back(unk_id_);
        return ids;
    }

    // Char-boundary offsets so substrings never split a UTF-8 sequence.
    std::vector<size_t> offsets;
    offsets.reserve(char_count + 1);
    for (size_t j = 0; j <= word.size();) {
        offsets.push_back(j);
        if (j == word.size()) break;
        NextCodePoint(word, j);
    }

    bool is_bad = false;
    size_t start = 0;  // index into offsets
    std::vector<int64_t> sub_ids;
    const size_t last = offsets.size() - 1;  // offsets.back() == word.size()
    while (start < last) {
        size_t end = last;
        int64_t cur_id = -1;
        std::string cur_sub;
        while (start < end) {
            std::string piece = word.substr(offsets[start], offsets[end] - offsets[start]);
            if (start > 0) piece = "##" + piece;
            auto it = vocab_.find(piece);
            if (it != vocab_.end()) {
                cur_id = it->second;
                break;
            }
            --end;  // shrink from the right (longest-match-first)
        }
        if (cur_id < 0) {
            is_bad = true;
            break;
        }
        sub_ids.push_back(cur_id);
        start = end;
    }

    if (is_bad) {
        ids.push_back(unk_id_);
    } else {
        ids.insert(ids.end(), sub_ids.begin(), sub_ids.end());
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Status WordPieceTokenizer::LoadVocab(const std::string& vocab_txt_path) {
    std::ifstream in(vocab_txt_path);
    if (!in.is_open()) {
        return Status::NotFound("WordPieceTokenizer: vocab file not found: " + vocab_txt_path);
    }
    vocab_.clear();
    std::string line;
    int64_t idx = 0;
    while (std::getline(in, line)) {
        // vocab.txt tokens never contain a trailing \r/\n; tolerate CRLF files.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        vocab_.emplace(line, idx);
        ++idx;
    }
    if (vocab_.empty()) {
        return Status::Internal("WordPieceTokenizer: empty vocab: " + vocab_txt_path);
    }

    auto resolve = [&](const char* tok, int* out) -> bool {
        auto it = vocab_.find(tok);
        if (it == vocab_.end()) return false;
        *out = static_cast<int>(it->second);
        return true;
    };
    if (!resolve("[CLS]", &cls_id_) || !resolve("[SEP]", &sep_id_) ||
        !resolve("[UNK]", &unk_id_) || !resolve("[PAD]", &pad_id_)) {
        return Status::Internal(
            "WordPieceTokenizer: vocab missing a required special token "
            "([CLS]/[SEP]/[UNK]/[PAD]): " + vocab_txt_path);
    }

    loaded_ = true;
    return Status::Ok();
}

Status WordPieceTokenizer::Encode(const std::string& text, int max_length,
                                  Encoded* output) const {
    if (output == nullptr) {
        return Status::InvalidArgument("WordPieceTokenizer::Encode: null output");
    }
    if (!loaded_) {
        return Status::Internal("WordPieceTokenizer::Encode: vocab not loaded");
    }
    if (max_length < 2) {
        return Status::InvalidArgument("WordPieceTokenizer::Encode: max_length must be >= 2");
    }

    output->input_ids.clear();
    output->attention_mask.clear();

    // Content tokens, truncated to max_length-2 to leave room for [CLS] / [SEP]
    // (truncation_side=right). We stop emitting once the budget is full so a huge
    // input does not build a giant intermediate vector.
    const size_t budget = static_cast<size_t>(max_length) - 2;
    std::vector<int64_t> content;
    content.reserve(budget);
    for (const std::string& word : BasicTokenize(text)) {
        if (content.size() >= budget) break;
        for (int64_t id : WordPieceEncode(word)) {
            if (content.size() >= budget) break;
            content.push_back(id);
        }
    }

    output->input_ids.reserve(content.size() + 2);
    output->input_ids.push_back(cls_id_);
    output->input_ids.insert(output->input_ids.end(), content.begin(), content.end());
    output->input_ids.push_back(sep_id_);

    // Single sequence, no padding here: attention_mask is all 1s.
    output->attention_mask.assign(output->input_ids.size(), 1);
    return Status::Ok();
}

}  // namespace cortrix::query
