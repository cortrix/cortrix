// UTF-8 helpers guarding the JSON contract (see include/cortrix/common/utf8_utils.h).
//
// Regression coverage: GET /api/v1/operations returned HTTP 500 with
// "[json.exception.type_error.316] incomplete UTF-8 string; last byte: 0x80"
// when operation_log.summary rows had been byte-truncated at 100 bytes in the
// middle of a multi-byte UTF-8 character. These tests pin both halves:
//   * truncation is character-boundary aligned (no new malformed text is stored),
//   * SanitizeUtf8 output always survives a STRICT nlohmann dump (legacy
//     malformed rows cannot break response serialization).

#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/utf8_utils.h"

namespace cortrix::common {
namespace {

using nlohmann::json;

// ---------------------------------------------------------------------------
// IsWellFormedUtf8 — reject set must match nlohmann's strict serializer
// (Unicode RPI #3 maximal subpart policy).
// ---------------------------------------------------------------------------

TEST(IsWellFormedUtf8, AcceptsValidText) {
    EXPECT_TRUE(IsWellFormedUtf8(""));
    EXPECT_TRUE(IsWellFormedUtf8("plain ascii 0123"));
    EXPECT_TRUE(IsWellFormedUtf8(
        "\xE4\xB8\xAD\xE6\x96\x87-query-\xE6\x91\x98\xE8\xA6\x81"));  // 3-byte UTF-8 seqs
    EXPECT_TRUE(IsWellFormedUtf8("\xC3\xA9\xC3\xBC"));                // 2-byte UTF-8 seqs
    EXPECT_TRUE(IsWellFormedUtf8("\xF0\x9F\x98\x80"));                // U+1F600 emoji (4-byte)
    EXPECT_TRUE(IsWellFormedUtf8("\xE0\xA0\x80"));                    // U+0800 boundary lead
    EXPECT_TRUE(IsWellFormedUtf8("\xF4\x8F\xBF\xBF"));                // U+10FFFF max code point
}

TEST(IsWellFormedUtf8, RejectsIllFormedSequences) {
    EXPECT_FALSE(IsWellFormedUtf8("\x80"));                  // stray continuation (incident byte)
    EXPECT_FALSE(IsWellFormedUtf8("abc\xC0\x80"));           // overlong NUL (C0/C1 never valid)
    EXPECT_FALSE(IsWellFormedUtf8("abc\xE0\x80\x80"));       // overlong 3-byte
    EXPECT_FALSE(IsWellFormedUtf8("\xED\xA0\x80"));          // UTF-16 surrogate U+D800
    EXPECT_FALSE(IsWellFormedUtf8("\xF4\x90\x80\x80"));      // > U+10FFFF
    EXPECT_FALSE(IsWellFormedUtf8("\xF5\x80\x80\x80"));      // F5..FF lead: never valid
    EXPECT_FALSE(IsWellFormedUtf8("\xE4\xB8\xAD\xE4\xB8"));  // truncated 3-byte seq at end
    EXPECT_FALSE(IsWellFormedUtf8("\xC2"));                  // truncated 2-byte lead at end
}

TEST(IsWellFormedUtf8, MatchesNlohmannStrictDump) {
    // Cross-check the accept set against nlohmann itself: every string this
    // helper calls ill-formed must make a strict dump throw, and every string
    // it accepts must dump without error. (One shared contract = no drift.)
    const std::string bad[] = {"\x80",     "\xC0\xAF", "\xED\xA0\xBF", "\xF4\x90\x80\x80",
                               "\xE4\xB8", "\xFF"};
    for (const auto& s : bad) {
        ASSERT_FALSE(IsWellFormedUtf8(s)) << s;
        json j;
        j["v"] = s;
        EXPECT_THROW(j.dump(), json::type_error);
    }
    const std::string good[] = {"ok", "\xE4\xB8\xAD\xE6\x96\x87", "\xF0\x9F\x98\x80"};
    for (const auto& s : good) {
        ASSERT_TRUE(IsWellFormedUtf8(s)) << s;
        json j;
        j["v"] = s;
        EXPECT_NO_THROW(j.dump());
    }
}

// ---------------------------------------------------------------------------
// Utf8SafeTruncate — the write-site fix (TruncateSummary 100B, query_wiring
// F18a 100B, last_error 200B, value_preview 100B).
// ---------------------------------------------------------------------------

TEST(Utf8SafeTruncate, NoOpWhenWithinBudget) {
    EXPECT_EQ(Utf8SafeTruncate("short", 10), "short");
    EXPECT_EQ(Utf8SafeTruncate("", 10), "");
    EXPECT_EQ(Utf8SafeTruncate("exact10!", 8), "exact10!");
}

TEST(Utf8SafeTruncate, NeverCutsMultiByteSequences) {
    const std::string cjk = "\xE7\xBB\x8F\xE9\xAA\x8C\xE5\xBA\x93";  // three 3-byte chars = 9 bytes
    EXPECT_EQ(Utf8SafeTruncate(cjk, 9), cjk);
    EXPECT_EQ(Utf8SafeTruncate(cjk, 8),
              "\xE7\xBB\x8F\xE9\xAA\x8C");  // two chars (6B) -- walks back 2
    EXPECT_EQ(Utf8SafeTruncate(cjk, 7), "\xE7\xBB\x8F\xE9\xAA\x8C");
    EXPECT_EQ(Utf8SafeTruncate(cjk, 6), "\xE7\xBB\x8F\xE9\xAA\x8C");
    EXPECT_EQ(Utf8SafeTruncate(cjk, 5), "\xE7\xBB\x8F");  // one char (3B)
    EXPECT_EQ(Utf8SafeTruncate(cjk, 2), "");              // budget smaller than first char
    EXPECT_EQ(Utf8SafeTruncate(cjk, 0), "");
}

TEST(Utf8SafeTruncate, HandlesFourByteSequences) {
    const std::string emoji =
        "\xF0\x9F\x98\x80"
        "x";  // 4-byte + ASCII
    EXPECT_EQ(Utf8SafeTruncate(emoji, 3), "");
    EXPECT_EQ(Utf8SafeTruncate(emoji, 4), "\xF0\x9F\x98\x80");
    EXPECT_EQ(Utf8SafeTruncate(emoji, 5), emoji);
}

TEST(Utf8SafeTruncate, RespectsByteBudgetAndWellFormedness) {
    // Random-ish CJK + ASCII mix: result must ALWAYS be <= max bytes and
    // well-formed (this is what kept the 100B summaries poisible before).
    std::string s;
    for (int i = 0; i < 80; ++i) {
        s += (i % 3 == 0) ? "\xE4\xB8\xAD" : (i % 3 == 1) ? "ab" : "\xF0\x9F\x98\x80";
    }
    for (size_t max = 0; max <= 40; ++max) {
        const std::string t = Utf8SafeTruncate(s, max);
        EXPECT_LE(t.size(), max);
        EXPECT_TRUE(IsWellFormedUtf8(t)) << "max=" << max;
        EXPECT_EQ(t, s.substr(0, t.size()));  // still a prefix of the input
    }
}

TEST(Utf8SafeTruncate, ReproducesIncidentByteCutAt100) {
    // Regression shape: a multi-byte query longer than kSummaryMaxBytes (100).
    // A naive substr(0, 100) lands mid-character (byte 100 is a continuation
    // of the sequence started at 99) -> strict dump throws type_error.316 on
    // the read path.
    std::string query;
    while (query.size() < 100)
        query += "\xE7\x94\xA8\xE6\x88\xB7\xE9\x97\xAE\xE9\xA2\x98";  // four 3-byte chars (12B)
    const std::string naive = query.substr(0, 100);
    ASSERT_EQ(naive.size(), 100);
    ASSERT_FALSE(IsWellFormedUtf8(naive));
    json jn;
    jn["summary"] = naive;
    EXPECT_THROW(jn.dump(), json::type_error);

    const std::string fixed = Utf8SafeTruncate(query, 100);
    EXPECT_LE(fixed.size(), 100u);
    EXPECT_TRUE(IsWellFormedUtf8(fixed));
    json jf;
    jf["summary"] = fixed;
    EXPECT_NO_THROW(jf.dump());
}

// ---------------------------------------------------------------------------
// Utf8SafeHeadTail -- agent_trace params/result truncation (head+mid+tail).
// ---------------------------------------------------------------------------

TEST(Utf8SafeHeadTail, AlignsBothSides) {
    const std::string s =
        "1234567890"
        "\xE7\xBB\x8F\xE9\xAA\x8C\xE5\xBA\x93"  // three 3-byte chars at offset 10..19
        "abcdef";
    const std::string out = Utf8SafeHeadTail(s, 12, "...", 5);
    // head: 12B budget cuts at offset 12 = mid of second 3-byte char (E9 AA 8C); walks back to 10.
    EXPECT_EQ(out.substr(0, 10), "1234567890");
    EXPECT_EQ(out.find("..."), 10u);
    // tail: 5B back from end -> offset 20 ("bcdef"), already on ASCII boundary:
    // test a mid-sequence tail cut as well below.
    EXPECT_EQ(out.substr(out.size() - 4), "cdef");
    EXPECT_TRUE(IsWellFormedUtf8(out));
}

TEST(Utf8SafeHeadTail, ShortStringsKeepWholeTail) {
    const std::string s = "short";
    EXPECT_EQ(Utf8SafeHeadTail(s, 2, "...", 10), "sh...short");  // tail budget >= size: full tail
    // (documented behavior: tail keeps whole string when tail_bytes >= size)
}

// ---------------------------------------------------------------------------
// SanitizeUtf8 — the read-site / untrusted-input fix. Output MUST be accepted
// by a strict dump, byte-for-byte like nlohmann's error_handler_t::replace.
// ---------------------------------------------------------------------------

TEST(SanitizeUtf8, IdentityOnValidText) {
    EXPECT_EQ(SanitizeUtf8(""), "");
    EXPECT_EQ(SanitizeUtf8("\xE4\xB8\xAD\xE6\x96\x87-search"), "\xE4\xB8\xAD\xE6\x96\x87-search");
    EXPECT_EQ(SanitizeUtf8("\xF0\x9F\x98\x80"), "\xF0\x9F\x98\x80");
}

TEST(SanitizeUtf8, ReplacesOneFFFDPerMaximalSubpart) {
    EXPECT_EQ(SanitizeUtf8("\x80"), "\xEF\xBF\xBD");  // lone 0x80 (incident byte)
    EXPECT_EQ(SanitizeUtf8("a\xC0"
                           "\x80"
                           "b"),
              "a\xEF\xBF\xBD\xEF\xBF\xBD"
              "b");                                       // C0 + stray 80 = 2 subparts
    EXPECT_EQ(SanitizeUtf8("\xE4\xB8"), "\xEF\xBF\xBD");  // E4 B8 = one maximal subpart -> one FFFD
    EXPECT_EQ(
        SanitizeUtf8("\xE4\xB8\xAD\xE4\xB8"),
        "\xE4\xB8\xAD\xEF\xBF\xBD");  // RPI#3: incomplete lead collapses to a single replacement
    EXPECT_EQ(SanitizeUtf8("\xED\xA0\x80"),
              "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // ED bad second -> 1 + two strays
    EXPECT_EQ(SanitizeUtf8("\xF5"), "\xEF\xBF\xBD");
}

TEST(SanitizeUtf8, MatchesNlohmannReplaceHandler) {
    // Our replacement bytes must equal what nlohmann itself would emit with
    // error_handler_t::replace — so a sanitized value re-dumped strictly is
    // byte-stable (idempotent through any later serialization).
    const std::string samples[] = {"\x80",         "a\xC0\xAF b", "\xE4\xB8", "x\xF4\x90\x80\x80y",
                                   "\xED\xA0\xBF", "\xE0\x80\xAF"};
    for (const auto& s : samples) {
        json j;
        j["v"] = s;
        const std::string theirs = j.dump(-1, ' ', false, json::error_handler_t::replace);
        json j2;
        j2["v"] = SanitizeUtf8(s);
        const std::string ours = j2.dump();  // strict — must not throw
        EXPECT_EQ(ours, theirs) << "sample mismatch for " << testing::PrintToString(s);
    }
}

TEST(SanitizeUtf8, IsIdempotent) {
    const std::string once = SanitizeUtf8("\xE4\xB8 a \x80 \xC0");
    EXPECT_EQ(SanitizeUtf8(once), once);
    EXPECT_TRUE(IsWellFormedUtf8(once));
}

TEST(SanitizeUtf8, HealsHistoricalPoisonRows) {
    // Simulate legacy stored rows: 100-byte summaries byte-cut mid-character.
    // The operation-log reader runs every TEXT column through SanitizeUtf8, so
    // EntryToJson + strict dump succeed instead of throwing type_error.316.
    std::string q;
    while (q.size() < 130) q += "\xE7\x94\xA8\xE6\x88\xB7\xE9\x97\xAE\xE9\xA2\x98";
    const std::string stored = q.substr(0, 100);  // legacy byte-cut row as stored in SQLite
    ASSERT_FALSE(IsWellFormedUtf8(stored));
    const std::string healed = SanitizeUtf8(stored);
    ASSERT_TRUE(IsWellFormedUtf8(healed));
    json body;
    body["items"] = json::array({json{{"summary", healed}}});
    EXPECT_NO_THROW(body.dump());
}

// ---------------------------------------------------------------------------
// Utf8SafeTruncateSanitized / Utf8SafeHeadTailSanitized /
// Utf8SafeHeadTailBudgeted -- the byte-budget contract.
//
// Sanitizing WIDENS a value: one ill-formed byte becomes a 3-byte U+FFFD.
// A byte cap applied before the sanitize therefore does NOT bound the stored
// bytes. These helpers apply the cap to the sanitized text and count the
// truncation marker inside that budget.
// ---------------------------------------------------------------------------

TEST(Utf8SafeTruncateSanitized, CapsTheSanitizedOutput) {
    // 512 ill-formed bytes sanitize to 1536 bytes; the cap must still hold.
    const std::string poison(512, static_cast<char>(0x80));
    const std::string out = Utf8SafeTruncateSanitized(poison, 512);
    EXPECT_LE(out.size(), 512u);
    EXPECT_TRUE(IsWellFormedUtf8(out));

    json j;
    j["v"] = out;
    EXPECT_NO_THROW(j.dump());
}

TEST(Utf8SafeTruncateSanitized, KeepsValidTextUnchanged) {
    EXPECT_EQ(Utf8SafeTruncateSanitized("hello", 10), "hello");
    EXPECT_EQ(Utf8SafeTruncateSanitized("hello", 3), "hel");
}

TEST(Utf8SafeHeadTailSanitized, CapsHeadAndTailIndependently) {
    const std::string poison(600, static_cast<char>(0x80));
    const std::string out = Utf8SafeHeadTailSanitized(poison, 400, "[truncated]", 100);
    EXPECT_LE(out.size(), 400u + std::string("[truncated]").size() + 100u);
    EXPECT_TRUE(IsWellFormedUtf8(out));
}

TEST(Utf8SafeHeadTailBudgeted, ReviewerCaseStaysInsideThe512ByteLimit) {
    // Upstream review: TruncateResult(std::string(512, char(0x80))) took the
    // early-return branch and produced 1536 bytes, violating the documented
    // 512-byte result-summary limit that the writer persists unchanged.
    const std::string poison(512, static_cast<char>(0x80));
    const std::string out = Utf8SafeHeadTailBudgeted(poison, 400, "[truncated]", 100, 512);
    EXPECT_LE(out.size(), 512u);
    EXPECT_TRUE(IsWellFormedUtf8(out));
    json j;
    j["result_summary"] = out;
    EXPECT_NO_THROW(j.dump());
}

TEST(Utf8SafeHeadTailBudgeted, LongInputStaysInsideTheBudgetWithMarker) {
    const std::string in(10000, 'z');
    const std::string out = Utf8SafeHeadTailBudgeted(in, 1536, "[...truncated...]", 512, 2048);
    EXPECT_LE(out.size(), 2048u);
    EXPECT_NE(out.find("[...truncated...]"), std::string::npos);
    EXPECT_TRUE(IsWellFormedUtf8(out));
}

TEST(Utf8SafeHeadTailBudgeted, ShortInputKeepsNoMarker) {
    const std::string in(100, 'y');
    EXPECT_EQ(Utf8SafeHeadTailBudgeted(in, 400, "[truncated]", 100, 512), in);
}

TEST(Utf8SafeHeadTailBudgeted, ShrinksTailBeforeHead) {
    // When the budget cannot hold head+mid+tail, the tail gives up bytes
    // first and the head keeps its documented priority.
    const std::string in(1000, 'x');
    const std::string out = Utf8SafeHeadTailBudgeted(in, 400, "[truncated]", 100, 450);
    EXPECT_LE(out.size(), 450u);
    EXPECT_EQ(out.substr(0, 400), std::string(400, 'x'));  // head untouched
    EXPECT_NE(out.find("[truncated]"), std::string::npos);
}

TEST(Utf8SafeHeadTailBudgeted, HoldsTheBudgetForEveryInputShape) {
    const std::string poison_byte(1, static_cast<char>(0x80));
    const std::string e4_lead(1, static_cast<char>(0xE4));
    const std::string inputs[] = {
        "",
        "plain ascii",
        std::string(300, 'a'),
        std::string(600, 'a'),
        std::string(600, static_cast<char>(0x80)),
        std::string(300, static_cast<char>(0xE4)),
        std::string(100, static_cast<char>(0xF0)),
        poison_byte + e4_lead,
        "\xE4\xB8\xAD\xE6\x96\x87",
    };
    for (const auto& in : inputs) {
        for (size_t cap : {1u, 3u, 7u, 64u, 100u, 256u, 512u, 2048u}) {
            const std::string out = Utf8SafeHeadTailBudgeted(in, 400, "[truncated]", 100, cap);
            EXPECT_LE(out.size(), cap) << "cap=" << cap;
            EXPECT_TRUE(IsWellFormedUtf8(out)) << "cap=" << cap;
        }
    }
}

}  // namespace
}  // namespace cortrix::common
