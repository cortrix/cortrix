#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cortrix/onnx/startup_validator.h"

// ONNX runtime branch coverage for startup_validator.cpp's protobuf WireReader path. The
// happy-path opset reads + range checks live in test_onnx_startup_validator.cpp;
// this file targets the malformed / edge wire-format branches that a real .onnx
// header could exercise but the well-formed fixtures don't:
//   - opset_import declared only under a CUSTOM domain → max_version fallback
//   - multiple opset entries → the version > max_version branch
//   - an opset entry that is a string field at the top level (SkipField LEN)
//   - fields with I64 / I32 wire types at the top level (SkipField fixed-width)
//   - a truncated LEN-delimited chunk → ReadLengthDelimited overflow → unreadable
//   - an unknown wire type → SkipField default → stop without desync
// All inputs are synthesized bytes; no binary model blobs are checked in.
namespace cortrix::onnx {
namespace {

void AppendVarint(std::vector<uint8_t>* out, uint64_t v) {
    while (v >= 0x80) {
        out->push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out->push_back(static_cast<uint8_t>(v));
}

void AppendTag(std::vector<uint8_t>* out, uint32_t field, uint32_t wire_type) {
    AppendVarint(out, (static_cast<uint64_t>(field) << 3) | wire_type);
}

// One OperatorSetIdProto { domain, version } as a LEN-delimited inner message.
std::vector<uint8_t> EncodeOpsetEntry(const std::string& domain, int64_t version) {
    std::vector<uint8_t> inner;
    if (!domain.empty()) {
        AppendTag(&inner, 1, 2);  // domain (string)
        AppendVarint(&inner, domain.size());
        inner.insert(inner.end(), domain.begin(), domain.end());
    }
    AppendTag(&inner, 2, 0);      // version (varint)
    AppendVarint(&inner, static_cast<uint64_t>(version));
    return inner;
}

void AppendOpsetImport(std::vector<uint8_t>* out, const std::string& domain,
                       int64_t version) {
    std::vector<uint8_t> entry = EncodeOpsetEntry(domain, version);
    AppendTag(out, 8, 2);  // opset_import = field 8, LEN message
    AppendVarint(out, entry.size());
    out->insert(out->end(), entry.begin(), entry.end());
}

std::string WriteTempModel(const std::string& name, const std::vector<uint8_t>& bytes) {
    std::string path =
        (std::filesystem::temp_directory_path() / ("cortrix_onnxvalidator_b_" + name + ".onnx")).string();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.close();
    return path;
}

struct TempModel {
    explicit TempModel(const std::string& name, const std::vector<uint8_t>& bytes)
        : path(WriteTempModel(name, bytes)) {}
    ~TempModel() { std::filesystem::remove(path); }
    std::string path;
};

// Only a custom-domain opset is declared (no default/ai.onnx) → ReadModelOpset
// falls back to the max version seen (default_domain_version stays -1, so the
// `default_domain_version >= 0 ? ... : max_version` ternary picks max_version).
TEST(StartupValidatorBranchesTest, CustomDomainOnlyUsesMaxFallback) {
    std::vector<uint8_t> bytes;
    AppendOpsetImport(&bytes, "com.microsoft", 15);
    TempModel m("customdomain", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 15);
}

// Two opset entries; the second has a higher version → the `version > max_version`
// branch updates max_version. With a default-domain entry present, the default
// wins regardless — here we make the DEFAULT one the larger to keep the chosen
// value deterministic while still exercising the multi-entry loop.
TEST(StartupValidatorBranchesTest, MultipleOpsetsDefaultPreferred) {
    std::vector<uint8_t> bytes;
    AppendOpsetImport(&bytes, "com.microsoft", 12);  // custom, lower
    AppendOpsetImport(&bytes, "", 18);               // default domain, higher
    TempModel m("multiopset", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 18);  // default-domain version chosen
}

// A custom domain larger than the default → default still chosen (default branch
// wins) but the max_version-updating branch is exercised by the larger custom one.
TEST(StartupValidatorBranchesTest, DefaultChosenEvenWhenCustomLarger) {
    std::vector<uint8_t> bytes;
    AppendOpsetImport(&bytes, "", 11);               // default domain
    AppendOpsetImport(&bytes, "com.microsoft", 99);  // custom, larger → updates max
    TempModel m("defaultwins", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 11);  // default domain preferred over the larger custom
}

// A top-level field with I64 wire type (1) before opset_import → SkipField's
// I64/Advance(8) arm is exercised while scanning to field 8.
TEST(StartupValidatorBranchesTest, SkipsI64FieldThenReadsOpset) {
    std::vector<uint8_t> bytes;
    AppendTag(&bytes, 5, 1);                 // some field, I64
    for (int i = 0; i < 8; ++i) bytes.push_back(0xAA);  // 8 fixed bytes
    AppendOpsetImport(&bytes, "", 17);
    TempModel m("i64skip", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 17);
}

// A top-level field with I32 wire type (5) → SkipField's I32/Advance(4) arm.
TEST(StartupValidatorBranchesTest, SkipsI32FieldThenReadsOpset) {
    std::vector<uint8_t> bytes;
    AppendTag(&bytes, 6, 5);                 // some field, I32
    for (int i = 0; i < 4; ++i) bytes.push_back(0xBB);
    AppendOpsetImport(&bytes, "", 16);
    TempModel m("i32skip", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 16);
}

// A top-level LEN-delimited string field (e.g. producer_name) before the opset →
// SkipField's LEN/ReadLengthDelimited arm.
TEST(StartupValidatorBranchesTest, SkipsTopLevelStringThenReadsOpset) {
    std::vector<uint8_t> bytes;
    const std::string producer = "pytorch";
    AppendTag(&bytes, 2, 2);                  // producer_name, string
    AppendVarint(&bytes, producer.size());
    bytes.insert(bytes.end(), producer.begin(), producer.end());
    AppendOpsetImport(&bytes, "ai.onnx", 19);
    TempModel m("strskip", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 19);
}

// An opset_import whose declared LEN exceeds the remaining bytes → the
// ReadLengthDelimited overflow branch sets ok_=false; no opset is read → the
// "no opset_import found" error (chosen < 0).
TEST(StartupValidatorBranchesTest, TruncatedLenChunkUnreadable) {
    std::vector<uint8_t> bytes;
    AppendTag(&bytes, 8, 2);          // opset_import, LEN
    AppendVarint(&bytes, 200);        // claims 200 bytes...
    bytes.push_back(0x10);            // ...but only 1 byte follows → overflow
    TempModel m("trunclen", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_ONNX_OPSET_INCOMPATIBLE"),
              std::string::npos);
}

// An unknown wire type (3 = group start, unhandled) → SkipField default arm sets
// ok_=false and stops scanning. No opset read → unreadable.
TEST(StartupValidatorBranchesTest, UnknownWireTypeStopsScan) {
    std::vector<uint8_t> bytes;
    AppendTag(&bytes, 4, 3);  // wire type 3 (unsupported) → default → stop
    TempModel m("unknownwt", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    EXPECT_FALSE(r.ok());
}

// A truncated varint (high bit set, then EOF) at the top level → ReadVarint sets
// ok_=false on the loop-exit-without-terminator path → scan stops, unreadable.
TEST(StartupValidatorBranchesTest, TruncatedVarintUnreadable) {
    std::vector<uint8_t> bytes = {0x80, 0x80};  // continuation bits, no terminator
    TempModel m("truncvarint", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    EXPECT_FALSE(r.ok());
}

// An opset entry with NO version field (only a domain) → ParseOpsetEntry returns
// got_version=false → the `&& version >= 0` guard short-circuits and the entry is
// ignored. With no other opset → unreadable.
TEST(StartupValidatorBranchesTest, OpsetEntryWithoutVersionIgnored) {
    // Inner message carries only a domain string, no version varint.
    std::vector<uint8_t> inner;
    AppendTag(&inner, 1, 2);
    const std::string dom = "ai.onnx";
    AppendVarint(&inner, dom.size());
    inner.insert(inner.end(), dom.begin(), dom.end());

    std::vector<uint8_t> bytes;
    AppendTag(&bytes, 8, 2);
    AppendVarint(&bytes, inner.size());
    bytes.insert(bytes.end(), inner.begin(), inner.end());
    TempModel m("noversion", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    EXPECT_FALSE(r.ok());
}

// An opset entry whose inner message carries an UNRELATED field (field 3) →
// ParseOpsetEntry's else-branch SkipField is exercised, version still read.
TEST(StartupValidatorBranchesTest, OpsetEntryWithExtraFieldStillReadsVersion) {
    std::vector<uint8_t> inner;
    AppendTag(&inner, 3, 0);          // an unrelated varint field inside the entry
    AppendVarint(&inner, 123);
    AppendTag(&inner, 2, 0);          // version
    AppendVarint(&inner, 14);

    std::vector<uint8_t> bytes;
    AppendTag(&bytes, 8, 2);
    AppendVarint(&bytes, inner.size());
    bytes.insert(bytes.end(), inner.begin(), inner.end());
    TempModel m("extrafield", bytes);
    auto r = StartupValidator::ReadModelOpset(m.path);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 14);
}

}  // namespace
}  // namespace cortrix::onnx
