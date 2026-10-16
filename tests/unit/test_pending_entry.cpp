#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/store/group_commit_writer.h"  // Crc32, to hand-build frames
#include "cortrix/store/pending_entry.h"

namespace cortrix::store {
namespace {

using State = PendingEntry::State;

// The CRC-covered body of a serialized frame: strip the 4B entry_size prefix and
// the trailing 4B CRC.
std::vector<uint8_t> BodyOf(const std::vector<uint8_t>& frame) {
    return std::vector<uint8_t>(frame.begin() + 4, frame.end() - 4);
}

// Frame a (mutated) body the way Serialize does: entry_size = body + CRC bytes,
// then the body, then CRC32 over the body — so the parser sees a well-formed
// record of the new shape and any rejection is about the content, not the frame.
std::vector<uint8_t> ReframeBody(const std::vector<uint8_t>& body) {
    const uint32_t crc = Crc32(body.data(), body.size());
    const uint32_t follow = static_cast<uint32_t>(body.size() + 4);
    std::vector<uint8_t> frame;
    for (int i = 0; i < 4; ++i) frame.push_back(static_cast<uint8_t>((follow >> (8 * i)) & 0xFF));
    frame.insert(frame.end(), body.begin(), body.end());
    for (int i = 0; i < 4; ++i) frame.push_back(static_cast<uint8_t>((crc >> (8 * i)) & 0xFF));
    return frame;
}

PendingEntry MakePending() {
    PendingEntry e;
    e.state = State::kPending;
    e.txn_id = 42;
    e.timestamp_ms = 1716998400123;  // arbitrary Unix ms
    e.doc_id = "01J0ABCDEFGHJKMNPQRSTVWXYZ";  // ULID-ish, 26 chars
    e.block_ids = {1, 2, 3, 1000, 9999};
    e.has_blob = true;  // v1.0.2: PENDING records carry the has_blob byte
    return e;
}

TEST(PendingEntryTest, PendingRoundTrip) {
    PendingEntry in = MakePending();
    std::vector<uint8_t> bytes = in.Serialize();

    auto out = PendingEntry::Deserialize(bytes);
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out->state, State::kPending);
    EXPECT_EQ(out->txn_id, in.txn_id);
    EXPECT_EQ(out->timestamp_ms, in.timestamp_ms);
    EXPECT_EQ(out->doc_id, in.doc_id);
    EXPECT_EQ(out->block_ids, in.block_ids);
    EXPECT_EQ(out->has_blob, in.has_blob);  // v1.0.2: has_blob round-trips
}

TEST(PendingEntryTest, CommittedRoundTrip) {
    // COMMITTED records carry no doc_id / block_ids; only txn_id matters.
    PendingEntry in;
    in.state = State::kCommitted;
    in.txn_id = 7;
    in.timestamp_ms = 123456789;
    in.doc_id = "ignored-on-wire";   // dropped by Serialize for terminal records
    in.block_ids = {5, 6, 7};        // dropped too

    auto out = PendingEntry::Deserialize(in.Serialize());
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out->state, State::kCommitted);
    EXPECT_EQ(out->txn_id, 7u);
    EXPECT_EQ(out->timestamp_ms, 123456789);
    EXPECT_TRUE(out->doc_id.empty());
    EXPECT_TRUE(out->block_ids.empty());
    EXPECT_FALSE(out->has_blob);  // terminal records carry no has_blob byte (§3.1)
}

TEST(PendingEntryTest, PendingHasBlobFalseRoundTrip) {
    // The blob-less case (watch_dir / CDC / memory): has_blob=false must survive
    // the round trip so Recover skips the blob check (§4.4, the §10.4 fix).
    PendingEntry in;
    in.state = State::kPending;
    in.txn_id = 11;
    in.doc_id = "blobless-doc";
    in.block_ids = {1, 2};
    in.has_blob = false;

    auto out = PendingEntry::Deserialize(in.Serialize());
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_FALSE(out->has_blob);
    EXPECT_EQ(out->block_ids, in.block_ids);
}

TEST(PendingEntryTest, HasBlobByteIsPendingOnly) {
    // The has_blob byte sits between block_ids and the CRC, PENDING only (§3.1
    // v1.0.2). A terminal record keeps its fixed 31-byte frame (no doc_id, no
    // block_ids, no has_blob byte); a PENDING frame is larger and parses cleanly.
    PendingEntry term;
    term.state = State::kCommitted;
    term.txn_id = 1;
    EXPECT_EQ(term.Serialize().size(), 31u);

    PendingEntry pend;
    pend.state = State::kPending;
    pend.txn_id = 1;
    pend.doc_id = "d";
    pend.block_ids = {9};
    pend.has_blob = true;
    EXPECT_GT(pend.Serialize().size(), 31u);
    EXPECT_TRUE(PendingEntry::Deserialize(pend.Serialize()).ok());
}

TEST(PendingEntryTest, ScopeByteRoundTripsDefaultsAndTolerates) {
    // The scope byte trails has_blob, PENDING only, and is the first OPTIONAL
    // trailing field: a record written before it existed ends at has_blob and
    // must read as document scope; an unknown value must also read as document,
    // never as corrupt — a rejected record truncates the log at that point and
    // would discard every later transaction's records over one unknown byte.
    PendingEntry in = MakePending();
    in.scope = PendingEntry::Scope::kBlocksOnly;
    const std::vector<uint8_t> with_scope = in.Serialize();

    // (1) blocks-only round-trips; a record built without touching scope reads
    //     as document.
    auto out = PendingEntry::Deserialize(with_scope);
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out->scope, PendingEntry::Scope::kBlocksOnly);
    EXPECT_TRUE(out->has_blob);
    auto dflt = PendingEntry::Deserialize(MakePending().Serialize());
    ASSERT_TRUE(dflt.ok()) << dflt.status().message();
    EXPECT_EQ(dflt->scope, PendingEntry::Scope::kDocument);

    // (2) A frame of the previous shape — has_blob as the last body byte, no
    //     scope byte — parses and reads as document. Built by hand from the
    //     current frame: drop the scope byte, recompute entry_size and the CRC.
    std::vector<uint8_t> legacy_body = BodyOf(with_scope);
    ASSERT_EQ(static_cast<int>(legacy_body.back()),
              static_cast<int>(PendingEntry::Scope::kBlocksOnly));
    legacy_body.pop_back();
    ASSERT_EQ(static_cast<int>(legacy_body.back()), 1);  // has_blob=true is last again
    const std::vector<uint8_t> legacy = ReframeBody(legacy_body);
    EXPECT_EQ(legacy.size() + 1, with_scope.size());
    auto old = PendingEntry::Deserialize(legacy);
    ASSERT_TRUE(old.ok()) << old.status().message();
    EXPECT_EQ(old->scope, PendingEntry::Scope::kDocument);
    EXPECT_TRUE(old->has_blob);
    EXPECT_EQ(old->block_ids, in.block_ids);
    EXPECT_EQ(old->doc_id, in.doc_id);

    // (3) An unknown scope value reads as document, not as corruption.
    std::vector<uint8_t> odd_body = BodyOf(with_scope);
    odd_body.back() = 7;
    auto odd = PendingEntry::Deserialize(ReframeBody(odd_body));
    ASSERT_TRUE(odd.ok()) << odd.status().message();
    EXPECT_EQ(odd->scope, PendingEntry::Scope::kDocument);
    EXPECT_TRUE(odd->has_blob);

    // (4) Terminal records carry no scope byte: still the fixed 31-byte frame,
    //     and they read as document.
    PendingEntry term;
    term.state = State::kRollback;
    term.txn_id = 1;
    EXPECT_EQ(term.Serialize().size(), 31u);
    auto t = PendingEntry::Deserialize(term.Serialize());
    ASSERT_TRUE(t.ok()) << t.status().message();
    EXPECT_EQ(t->scope, PendingEntry::Scope::kDocument);
}

TEST(PendingEntryTest, RollbackRoundTrip) {
    PendingEntry in;
    in.state = State::kRollback;
    in.txn_id = 99;
    in.timestamp_ms = -1;  // sign round-trips through u64 encoding

    auto out = PendingEntry::Deserialize(in.Serialize());
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out->state, State::kRollback);
    EXPECT_EQ(out->txn_id, 99u);
    EXPECT_EQ(out->timestamp_ms, -1);
}

TEST(PendingEntryTest, EmptyDocAndNoBlocks) {
    PendingEntry in;
    in.state = State::kPending;
    in.txn_id = 1;
    in.timestamp_ms = 0;
    // empty doc_id, empty block_ids — boundary of the variable-length fields.

    auto out = PendingEntry::Deserialize(in.Serialize());
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_TRUE(out->doc_id.empty());
    EXPECT_TRUE(out->block_ids.empty());
}

TEST(PendingEntryTest, CrcValid) {
    // A clean serialization deserializes without a corruption error.
    auto out = PendingEntry::Deserialize(MakePending().Serialize());
    EXPECT_TRUE(out.ok());
}

TEST(PendingEntryTest, CrcTamperedInBody) {
    std::vector<uint8_t> bytes = MakePending().Serialize();
    // Flip a bit inside the CRC-covered body (just past the 4B size prefix).
    bytes[6] ^= 0x01;

    auto out = PendingEntry::Deserialize(bytes);
    EXPECT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("CX_ERR_PWL_CORRUPTED"), std::string::npos);
}

TEST(PendingEntryTest, CrcTamperedChecksumBytes) {
    std::vector<uint8_t> bytes = MakePending().Serialize();
    bytes.back() ^= 0xFF;  // corrupt the stored CRC itself

    auto out = PendingEntry::Deserialize(bytes);
    EXPECT_FALSE(out.ok());
}

TEST(PendingEntryTest, RejectsTruncatedFrame) {
    std::vector<uint8_t> bytes = MakePending().Serialize();
    bytes.resize(bytes.size() - 3);  // chop the tail → entry_size overruns buffer

    auto out = PendingEntry::Deserialize(bytes);
    EXPECT_FALSE(out.ok());
}

TEST(PendingEntryTest, RejectsTrailingBytes) {
    std::vector<uint8_t> bytes = MakePending().Serialize();
    bytes.push_back(0x00);  // extra byte after a complete single record

    auto out = PendingEntry::Deserialize(bytes);
    EXPECT_FALSE(out.ok());  // single-record Deserialize must consume exactly all
}

TEST(PendingEntryTest, RejectsUnknownEntryType) {
    // Hand-build a frame whose entry_type is out of range but CRC is valid, to
    // prove the parser validates the enum and not just the checksum.
    PendingEntry in;
    in.state = static_cast<State>(7);  // invalid (valid range 1..3)
    in.txn_id = 1;
    auto out = PendingEntry::Deserialize(in.Serialize());
    EXPECT_FALSE(out.ok());
}

TEST(PendingEntryTest, DeserializeFromAdvancesOffset) {
    // Two records back-to-back in one buffer; DeserializeFrom should walk both.
    PendingEntry a = MakePending();
    PendingEntry b;
    b.state = State::kCommitted;
    b.txn_id = 43;
    b.timestamp_ms = 7;

    std::vector<uint8_t> buf = a.Serialize();
    std::vector<uint8_t> bb = b.Serialize();
    buf.insert(buf.end(), bb.begin(), bb.end());

    size_t off = 0;
    auto r1 = PendingEntry::DeserializeFrom(buf.data(), buf.size(), &off);
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1->txn_id, 42u);
    EXPECT_GT(off, 0u);

    auto r2 = PendingEntry::DeserializeFrom(buf.data(), buf.size(), &off);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2->txn_id, 43u);
    EXPECT_EQ(off, buf.size());
}

TEST(PendingEntryTest, LargeBlockList) {
    PendingEntry in;
    in.state = State::kPending;
    in.txn_id = 5;
    in.doc_id = "doc-with-many-blocks";
    for (int i = 0; i < 500; ++i) in.block_ids.push_back(i * 7 + 1);

    auto out = PendingEntry::Deserialize(in.Serialize());
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out->block_ids.size(), 500u);
    EXPECT_EQ(out->block_ids[499], 499 * 7 + 1);
}

}  // namespace
}  // namespace cortrix::store
