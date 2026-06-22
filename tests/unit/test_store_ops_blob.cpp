// test_store_ops_blob.cpp — BREADTH matrices for CortrixBlobLocal store/load/del
// over payload + namespace + roundtrip + missing-path matrices. Complements
// test_cortrix_blob_local.cpp (which covers the named one-off cases) with
// parameterized sweeps.
//
// Deterministic: temp blob dir under temp_directory_path(), no network/LLM.
// gtest suite/fixture names are GLOBAL strings — every name here is prefixed
// BlobLocalOps* to stay globally unique (no clash with BlobLocalTest etc.).
// API verified against include/cortrix/store/cortrix_blob_local.h:
//   int store(ns, doc_id, const void* data, size_t len);  // 0 ok / -1 err
//   int load(ns, doc_id, std::vector<uint8_t>& out);      // 0 ok / -2 missing
//   int del(ns, doc_id);                                   // 0 ok / -2 missing

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/store/cortrix_blob_local.h"

using namespace cortrix;
namespace fs = std::filesystem;

namespace {

class BlobLocalOpsBase : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique per PROCESS (gtest_discover_tests runs each case in its own
        // process; rand() is unseeded and identical across them -> all
        // concurrent processes picked the same dir and clobbered each other's
        // blob .tmp files under -j>1, rename ENOENT) AND per SetUp.
        static std::atomic<unsigned> seq{0};
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_blob_ops_" + std::to_string(getpid()) + "_" +
                     std::to_string(seq.fetch_add(1)));
        fs::create_directories(test_dir_);
        blob_ = std::make_unique<CortrixBlobLocal>(test_dir_.string());
    }
    void TearDown() override {
        blob_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }
    int Store(const std::string& ns, const std::string& id,
              const std::vector<uint8_t>& d) {
        return blob_->store(ns, id, d.data(), d.size());
    }
    fs::path test_dir_;
    std::unique_ptr<CortrixBlobLocal> blob_;
};

// =============================================================================
// MATRIX 1 — store/load roundtrip over a payload matrix.
// =============================================================================
struct PayloadCase {
    std::string name;
    size_t len;
    uint8_t fill_mode;  // 0=zeros, 1=0xFF, 2=i%256, 3=i%251 prime, 4=high-bit
};

std::vector<uint8_t> MakePayload(const PayloadCase& p) {
    std::vector<uint8_t> v(p.len);
    for (size_t i = 0; i < p.len; ++i) {
        switch (p.fill_mode) {
            case 0: v[i] = 0x00; break;
            case 1: v[i] = 0xFF; break;
            case 2: v[i] = static_cast<uint8_t>(i % 256); break;
            case 3: v[i] = static_cast<uint8_t>(i % 251); break;
            default: v[i] = static_cast<uint8_t>(0x80 | (i & 0x7F)); break;
        }
    }
    return v;
}

class BlobLocalOpsPayload : public BlobLocalOpsBase,
                            public ::testing::WithParamInterface<PayloadCase> {};

TEST_P(BlobLocalOpsPayload, StoreLoadRoundtripExact) {
    const auto& p = GetParam();
    std::vector<uint8_t> data = MakePayload(p);
    const std::string id = "01JBLOBPAYLOAD000000000001";
    ASSERT_EQ(Store("payload", id, data), 0) << p.name;

    std::vector<uint8_t> out;
    ASSERT_EQ(blob_->load("payload", id, out), 0) << p.name;
    EXPECT_EQ(out.size(), data.size());
    EXPECT_EQ(out, data) << "byte-exact roundtrip failed for " << p.name;
}

TEST_P(BlobLocalOpsPayload, StoreThenDeleteThenLoadMissing) {
    const auto& p = GetParam();
    std::vector<uint8_t> data = MakePayload(p);
    const std::string id = "01JBLOBPAYLOAD000000000002";
    ASSERT_EQ(Store("payload", id, data), 0) << p.name;
    EXPECT_EQ(blob_->del("payload", id), 0);
    std::vector<uint8_t> out;
    EXPECT_EQ(blob_->load("payload", id, out), -2) << "load after delete";
}

INSTANTIATE_TEST_SUITE_P(
    Payloads, BlobLocalOpsPayload,
    ::testing::Values(
        PayloadCase{"empty", 0, 0},
        PayloadCase{"one_byte_zero", 1, 0},
        PayloadCase{"one_byte_ff", 1, 1},
        PayloadCase{"two_bytes", 2, 2},
        PayloadCase{"three_bytes_high", 3, 4},
        PayloadCase{"small_seq", 17, 2},
        PayloadCase{"seq_127", 127, 2},
        PayloadCase{"page_256", 256, 2},
        PayloadCase{"page_257", 257, 3},
        PayloadCase{"half_k_zeros", 512, 0},
        PayloadCase{"all_zeros_1k", 1024, 0},
        PayloadCase{"all_ff_1k", 1024, 1},
        PayloadCase{"seq_1k", 1024, 2},
        PayloadCase{"prime_4k", 4096, 3},
        PayloadCase{"highbit_8k", 8192, 4},
        PayloadCase{"zeros_16k", 16 * 1024, 0},
        PayloadCase{"highbit_64k", 64 * 1024, 4},
        PayloadCase{"seq_128k", 128 * 1024, 2},
        PayloadCase{"large_1m", 1024 * 1024, 3},
        PayloadCase{"odd_size", 65537, 2},
        PayloadCase{"prime_odd_99991", 99991, 3}),
    [](const ::testing::TestParamInfo<PayloadCase>& i) { return i.param.name; });

// =============================================================================
// MATRIX 2 — namespace isolation: same doc_id, distinct payloads per ns.
// =============================================================================
class BlobLocalOpsNamespace : public BlobLocalOpsBase,
                              public ::testing::WithParamInterface<std::string> {};

TEST_P(BlobLocalOpsNamespace, NamespacesDoNotClobberSameDocId) {
    const std::string ns = GetParam();
    const std::string other = "second_ns";
    const std::string id = "01JBLOBNSISOLATE0000000001";
    std::vector<uint8_t> a(64, 0xA1);
    std::vector<uint8_t> b(96, 0xB2);

    ASSERT_EQ(Store(ns, id, a), 0);
    // Skip the degenerate ns==other pair (would be an overwrite, not isolation).
    if (ns != other) {
        ASSERT_EQ(Store(other, id, b), 0);
        std::vector<uint8_t> out;
        ASSERT_EQ(blob_->load(ns, id, out), 0);
        EXPECT_EQ(out, a) << "ns " << ns << " payload corrupted by other ns";
        ASSERT_EQ(blob_->load(other, id, out), 0);
        EXPECT_EQ(out, b);
    }

    // Deleting in one ns leaves the same id in another ns intact.
    if (ns != other) {
        EXPECT_EQ(blob_->del(ns, id), 0);
        std::vector<uint8_t> out;
        EXPECT_EQ(blob_->load(ns, id, out), -2);     // gone in ns
        EXPECT_EQ(blob_->load(other, id, out), 0);   // survives in other
    }
}

INSTANTIATE_TEST_SUITE_P(
    NsNames, BlobLocalOpsNamespace,
    ::testing::Values("default", "tenant-a", "tenant-b", "ns_underscore",
                      "MixedCase", "UPPER", "ns.with.dots", "a", "ab", "abc",
                      "ns123", "123ns", "very_long_namespace_name_12345"),
    [](const ::testing::TestParamInfo<std::string>& i) {
        std::string s = i.param;
        for (auto& c : s) if (!std::isalnum((unsigned char)c)) c = '_';
        return s;
    });

// =============================================================================
// MATRIX 3 — overwrite: a second store at the same (ns,doc_id) replaces content.
// Parameterized over (first_len, second_len) so grow/shrink/same are covered.
// =============================================================================
struct OverwriteCase {
    std::string name;
    size_t first_len;
    size_t second_len;
};

class BlobLocalOpsOverwrite : public BlobLocalOpsBase,
                              public ::testing::WithParamInterface<OverwriteCase> {};

TEST_P(BlobLocalOpsOverwrite, SecondStoreReplacesContent) {
    const auto& p = GetParam();
    const std::string id = "01JBLOBOVERWRITE0000000001";
    std::vector<uint8_t> v1(p.first_len, 0x11);
    std::vector<uint8_t> v2(p.second_len, 0x22);

    ASSERT_EQ(Store("ow", id, v1), 0);
    ASSERT_EQ(Store("ow", id, v2), 0);

    std::vector<uint8_t> out;
    ASSERT_EQ(blob_->load("ow", id, out), 0);
    EXPECT_EQ(out, v2) << "overwrite did not replace: " << p.name;

    // No stray temp file left behind: only the .raw file in the bucket dir.
    // (Locate the bucket via a directory walk; the impl uses a hash bucket.)
    for (auto it = fs::recursive_directory_iterator(test_dir_);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file()) {
            EXPECT_EQ(it->path().extension(), ".raw")
                << "stray non-.raw file: " << it->path();
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    GrowShrink, BlobLocalOpsOverwrite,
    ::testing::Values(
        OverwriteCase{"grow", 8, 128},
        OverwriteCase{"grow_large", 16, 4096},
        OverwriteCase{"shrink", 128, 8},
        OverwriteCase{"shrink_large", 4096, 16},
        OverwriteCase{"same_size", 64, 64},
        OverwriteCase{"same_size_large", 4096, 4096},
        OverwriteCase{"to_empty", 64, 0},
        OverwriteCase{"from_empty", 0, 64},
        OverwriteCase{"empty_to_empty", 0, 0},
        OverwriteCase{"one_to_one", 1, 1}),
    [](const ::testing::TestParamInfo<OverwriteCase>& i) { return i.param.name; });

// =============================================================================
// MATRIX 4 — missing-path semantics: load/del on a never-stored blob.
// Parameterized over (ns, doc_id) variety; all must return -2.
// =============================================================================
struct MissingCase {
    std::string name;
    std::string ns;
    std::string doc_id;
};

class BlobLocalOpsMissing : public BlobLocalOpsBase,
                            public ::testing::WithParamInterface<MissingCase> {};

TEST_P(BlobLocalOpsMissing, LoadMissingReturnsMinusTwo) {
    const auto& p = GetParam();
    std::vector<uint8_t> out;
    EXPECT_EQ(blob_->load(p.ns, p.doc_id, out), -2) << p.name;
    EXPECT_TRUE(out.empty());
}

TEST_P(BlobLocalOpsMissing, DeleteMissingReturnsMinusTwo) {
    const auto& p = GetParam();
    EXPECT_EQ(blob_->del(p.ns, p.doc_id), -2) << p.name;
}

INSTANTIATE_TEST_SUITE_P(
    NeverStored, BlobLocalOpsMissing,
    ::testing::Values(
        MissingCase{"fresh_ns", "neverns", "01JBLOBMISS00000000000001"},
        MissingCase{"existing_ns_other_id", "default", "01JBLOBMISS00000000000002"},
        MissingCase{"empty_doc_id", "default", ""},
        MissingCase{"long_doc_id", "default",
                    "01JBLOBMISS000000000000000000000000000003"}),
    [](const ::testing::TestParamInfo<MissingCase>& i) { return i.param.name; });

// =============================================================================
// MATRIX 5 — delete idempotency: store, del (ok), del again (missing).
// Parameterized over payload size so empty/non-empty both covered.
// =============================================================================
class BlobLocalOpsDeleteTwice : public BlobLocalOpsBase,
                                public ::testing::WithParamInterface<size_t> {};

TEST_P(BlobLocalOpsDeleteTwice, SecondDeleteIsMissing) {
    const size_t len = GetParam();
    const std::string id = "01JBLOBDELTWICE00000000001";
    std::vector<uint8_t> data(len, 0x5A);
    ASSERT_EQ(Store("dt", id, data), 0);
    EXPECT_EQ(blob_->del("dt", id), 0);
    EXPECT_EQ(blob_->del("dt", id), -2);
    std::vector<uint8_t> out;
    EXPECT_EQ(blob_->load("dt", id, out), -2);
}

INSTANTIATE_TEST_SUITE_P(Sizes, BlobLocalOpsDeleteTwice,
                         ::testing::Values<size_t>(0, 1, 2, 16, 32, 256, 1024,
                                                   4096, 65536));

// =============================================================================
// MATRIX 6 — nullptr-with-zero-length store creates an empty, loadable blob.
// =============================================================================
class BlobLocalOpsNullStore : public BlobLocalOpsBase,
                              public ::testing::WithParamInterface<std::string> {};

TEST_P(BlobLocalOpsNullStore, NullZeroCreatesEmptyBlob) {
    const std::string id = GetParam();
    EXPECT_EQ(blob_->store("nul", id, nullptr, 0), 0);
    std::vector<uint8_t> out;
    EXPECT_EQ(blob_->load("nul", id, out), 0);
    EXPECT_TRUE(out.empty());
}

INSTANTIATE_TEST_SUITE_P(
    Ids, BlobLocalOpsNullStore,
    ::testing::Values(std::string("01JBLOBNULL000000000000001"),
                      std::string("01JBLOBNULL000000000000256"),
                      std::string("01JBLOBNULL000000000000512")),
    [](const ::testing::TestParamInfo<std::string>& i) {
        return "id_" + i.param.substr(i.param.size() - 3);
    });

}  // namespace
