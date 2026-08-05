// BREADTH store roundtrip matrices for CortrixStoreSqlite -- BLOCK + PARENT side.
//
// Exhaustive field-value round-trip + NULL-sentinel coverage of the block CRUD
// surface (block_insert / block_get / block_get_by_doc / block_count /
// block_count_by_doc) and the parents table (parent_insert / parent_get), beyond
// the hand-written cases in test_cortrix_store_sqlite.cpp.
//
// Deterministic: each fixture owns a fresh temp-file SQLite store via Open().
// FK rule honored: blocks.doc_id -> documents.doc_id, so every fixture creates a
// parent DOCUMENT (whose minted doc_id all blocks reference) before inserting any
// block. blocks.data is NOT NULL, so every block gets a non-empty BlockBuild blob.
// CJK test DATA is injected via \x UTF-8 escapes; all comments are English.
//
// SUITE NAMING: every suite/fixture is globally unique, prefixed `BlockRoundtrip*`
// / `ParentRoundtrip*`, so it never collides with StoreSqliteTest, DocRoundtrip*,
// or any other linked test binary.

#include <gtest/gtest.h>
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace cortrix;
namespace fs = std::filesystem;

namespace {

// UTF-8 bytes for CJK strings as \x escapes (file stays ASCII).
const std::string kCjkContent  =
    "\xe8\xbf\x99\xe6\x98\xaf\xe6\xb5\x8b\xe8\xaf\x95";  // "this is a test"
const std::string kCjkMetaJson =
    "{\"ner\":[\"\xe5\x8c\x97\xe4\xba\xac\"]}";  // {"ner":["<Beijing CJK>"]}

// Fixture: fresh temp store + one parent document whose id all blocks reference.
class BlockRoundtripFixture : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cortrix_blk_rt_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir_);
        db_path_ = (dir_ / "blk_rt.db").string();
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);

        CortrixDoc d;
        d.source_type = "blk_rt_owner";
        d.source_path = "owner/doc";
        ASSERT_EQ(store_->doc_create(d), 0);
        ASSERT_FALSE(d.doc_id.empty());
        doc_id_ = d.doc_id;
    }
    void TearDown() override {
        if (store_) store_->Close();
        store_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    // Base block: valid FK, non-empty data, sensible defaults. Callers tweak fields.
    CortrixBlock BaseBlock(int chunk_index, const std::string& text = "payload") {
        CortrixBlock b;
        b.doc_id = doc_id_;
        b.chunk_index = chunk_index;
        b.block_type = kBlockFile;
        b.processing_level = kLevelL2;
        b.data = BlockBuild(kBlockFile, kLevelL2, text, "", "", 0, 0);
        b.content_text = text;
        return b;
    }
    fs::path dir_;
    std::string db_path_;
    std::string doc_id_;
    std::unique_ptr<CortrixStoreSqlite> store_;
};

// ============================================================================
// (A) block_type matrix: every CortrixBlockType round-trips through
//     block_insert/block_get unchanged.
// ============================================================================

class BlockRoundtripTypeMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<int> {};

TEST_P(BlockRoundtripTypeMatrix, BlockTypeRoundTrips) {
    const int bt = GetParam();
    CortrixBlock b = BaseBlock(0);
    b.block_type = bt;
    ASSERT_EQ(store_->block_insert(b), 0);
    ASSERT_GT(b.block_id, 0u);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.block_type, bt) << "block_type=" << bt;
}

INSTANTIATE_TEST_SUITE_P(
    BlockTypes, BlockRoundtripTypeMatrix,
    ::testing::Values(kBlockFile, kBlockScan, kBlockDatabase, kBlockAudio,
                      kBlockVideo, kBlockImage, kBlockMemory, kBlockMeta,
                      kBlockHypeQuestion, kBlockDocSummary));

// ============================================================================
// (B) processing_level matrix: every ProcessingLevel round-trips.
// ============================================================================

class BlockRoundtripLevelMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<int> {};

TEST_P(BlockRoundtripLevelMatrix, ProcessingLevelRoundTrips) {
    const int lvl = GetParam();
    CortrixBlock b = BaseBlock(0);
    b.processing_level = lvl;
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.processing_level, lvl) << "level=" << lvl;
}

INSTANTIATE_TEST_SUITE_P(
    BlockLevels, BlockRoundtripLevelMatrix,
    ::testing::Values(kLevelL0, kLevelL1, kLevelL2, kLevelL3, kLevelL4));

// ============================================================================
// (C) content_text matrix: ASCII / empty(NULL) / CJK / special chars / large.
//     Empty content_text is stored as SQL NULL and reads back empty.
// ============================================================================

struct ContentTextCase {
    std::string text;
    const char* label;
};

class BlockRoundtripContentTextMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<ContentTextCase> {};

TEST_P(BlockRoundtripContentTextMatrix, ContentTextRoundTrips) {
    const ContentTextCase& c = GetParam();
    CortrixBlock b = BaseBlock(0, "ignored");
    b.content_text = c.text;
    // Keep data non-empty regardless (data is NOT NULL); blob text need not match.
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.content_text, c.text) << c.label;
}

static std::string BigText() { return std::string(48 * 1024, 'q'); }

INSTANTIATE_TEST_SUITE_P(
    BlockContentText, BlockRoundtripContentTextMatrix,
    ::testing::Values(
        ContentTextCase{"plain ascii text", "ascii"},
        ContentTextCase{"", "empty_null"},
        ContentTextCase{kCjkContent, "cjk"},
        ContentTextCase{"O'Reilly's \"book\" <b>&</b>\nnl\ttab", "special"},
        ContentTextCase{"single", "single"},
        ContentTextCase{BigText(), "large48k"},
        ContentTextCase{"   leading and trailing   ", "whitespace"},
        ContentTextCase{"line1\nline2\nline3", "multiline"},
        ContentTextCase{"a", "single_char"},
        ContentTextCase{"emoji-free unicode: cafe", "ascii_unicodeish"},
        ContentTextCase{"semicolons; and -- comments /* */", "sql_lookalikes"},
        ContentTextCase{"\t\n\r", "only_whitespace_chars"}));

// ============================================================================
// (D) data (BLOB) matrix: payloads of varied size round-trip byte-for-byte. data
//     is NOT NULL so the empty-vector case is NOT included here (it would fail
//     the insert); the smallest payload is a minimal BlockBuild header.
// ============================================================================

struct BlobCase {
    std::vector<uint8_t> data;
    const char* label;
};

class BlockRoundtripBlobMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<BlobCase> {};

TEST_P(BlockRoundtripBlobMatrix, DataBlobRoundTrips) {
    const BlobCase& c = GetParam();
    CortrixBlock b = BaseBlock(0);
    b.data = c.data;
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    ASSERT_EQ(g.data.size(), c.data.size()) << c.label;
    EXPECT_EQ(g.data, c.data) << c.label;
}

static std::vector<uint8_t> AllBytePattern() {
    std::vector<uint8_t> v(256);
    for (int i = 0; i < 256; ++i) v[i] = static_cast<uint8_t>(i);  // incl. 0x00
    return v;
}
static std::vector<uint8_t> RawBytes(std::initializer_list<int> bs) {
    std::vector<uint8_t> v;
    for (int b : bs) v.push_back(static_cast<uint8_t>(b));
    return v;
}
static std::vector<uint8_t> BigBlob() {
    return std::vector<uint8_t>(70 * 1024, 0xCD);
}

INSTANTIATE_TEST_SUITE_P(
    BlockBlobs, BlockRoundtripBlobMatrix,
    ::testing::Values(
        BlobCase{BlockBuild(kBlockFile, kLevelL1, "", "", "", 0, 0), "header_only"},
        BlobCase{BlockBuild(kBlockFile, kLevelL3, "with content", "", "", 0, 0), "header_content"},
        BlobCase{RawBytes({1}), "one_byte"},
        BlobCase{RawBytes({0x00, 0xFF, 0x00, 0xFF}), "embedded_nulls"},
        BlobCase{AllBytePattern(), "all256_bytes"},
        BlobCase{BigBlob(), "large70k"}));

// ============================================================================
// (E) parent-child chunking child-column NULL-sentinel matrix: child_id/parent_id (empty=NULL),
//     token_count/parent_offset (-1=NULL), metadata_json (empty=NULL). Each case
//     drives the four sentinels independently and asserts the read-back.
// ============================================================================

struct ChildColCase {
    std::string child_id;       // "" => NULL
    std::string parent_id;      // "" => NULL
    int64_t     token_count;    // -1 => NULL
    int64_t     parent_offset;  // -1 => NULL
    std::string metadata_json;  // "" => NULL
    const char* label;
};

class BlockRoundtripChildColMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<ChildColCase> {};

TEST_P(BlockRoundtripChildColMatrix, ChildColumnsRoundTrip) {
    const ChildColCase& c = GetParam();
    CortrixBlock b = BaseBlock(0, "child payload");
    b.block_type = kBlockFile;      // child keeps source modality (no kBlockChild)
    b.processing_level = kLevelL3;
    b.child_id = c.child_id;
    b.parent_id = c.parent_id;
    b.token_count = c.token_count;
    b.parent_offset = c.parent_offset;
    b.metadata_json = c.metadata_json;
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.child_id, c.child_id) << c.label;
    EXPECT_EQ(g.parent_id, c.parent_id) << c.label;
    EXPECT_EQ(g.token_count, c.token_count) << c.label;
    EXPECT_EQ(g.parent_offset, c.parent_offset) << c.label;
    EXPECT_EQ(g.metadata_json, c.metadata_json) << c.label;

    // Same assertions through the batch read path.
    std::vector<CortrixBlock> blocks;
    ASSERT_EQ(store_->block_get_by_doc(doc_id_, blocks), 0);
    ASSERT_EQ(blocks.size(), 1u) << c.label;
    EXPECT_EQ(blocks[0].child_id, c.child_id) << c.label;
    EXPECT_EQ(blocks[0].parent_id, c.parent_id) << c.label;
    EXPECT_EQ(blocks[0].token_count, c.token_count) << c.label;
    EXPECT_EQ(blocks[0].parent_offset, c.parent_offset) << c.label;
    EXPECT_EQ(blocks[0].metadata_json, c.metadata_json) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    BlockChildCols, BlockRoundtripChildColMatrix,
    ::testing::Values(
        // all-NULL (a plain non-child block)
        ChildColCase{"", "", -1, -1, "", "all_null"},
        // fully-populated child row
        ChildColCase{"01HXCHILD0000000000000001", "01HXPARENT000000000000001",
                     42, 7, R"({"ner":["Acme"]})", "all_set"},
        // only child_id present
        ChildColCase{"01HXCHILD0000000000000002", "", -1, -1, "", "child_id_only"},
        // only parent_id present
        ChildColCase{"", "01HXPARENT000000000000002", -1, -1, "", "parent_id_only"},
        // only token_count present
        ChildColCase{"", "", 1024, -1, "", "token_only"},
        // only parent_offset present
        ChildColCase{"", "", -1, 256, "", "offset_only"},
        // only metadata_json present (+ CJK)
        ChildColCase{"", "", -1, -1, kCjkMetaJson, "meta_cjk_only"},
        // zero (not -1) is a real value, must NOT become NULL
        ChildColCase{"01HXCHILD0000000000000003", "01HXPARENT000000000000003",
                     0, 0, "{}", "zero_values"},
        // large token/offset values
        ChildColCase{"01HXCHILD0000000000000004", "01HXPARENT000000000000004",
                     9223372036854775807LL, 2147483648LL, R"({"big":true})", "large_ints"},
        // metadata present but ids null
        ChildColCase{"", "", 5, 9, R"({"only":"meta+counts"})", "counts_meta_no_ids"},
        // child + parent ids, no counts/meta
        ChildColCase{"01HXCHILD0000000000000005", "01HXPARENT000000000000005",
                     -1, -1, "", "ids_only"},
        // token_count present, offset null, with meta
        ChildColCase{"01HXCHILD0000000000000006", "", 333, -1, R"({"t":333})", "child_token_meta"},
        // offset present, token null
        ChildColCase{"", "01HXPARENT000000000000006", -1, 1000, R"({"o":1000})", "parent_offset_meta"},
        // both ids + large meta json
        ChildColCase{"01HXCHILD0000000000000007", "01HXPARENT000000000000007",
                     1, 1, R"({"arr":[1,2,3,4,5],"nested":{"x":"y"}})", "rich_meta"},
        // 1-token, 1-offset boundary
        ChildColCase{"01HXCHILD0000000000000008", "01HXPARENT000000000000008",
                     1, 0, "{}", "boundary_one_zero"}));

// ============================================================================
// (F) content_hash BLOB matrix: empty(NULL) / 16B binary / 32B / embedded NULs.
// ============================================================================

struct HashCase {
    std::string hash;   // raw bytes; "" => NULL
    const char* label;
};

class BlockRoundtripContentHashMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<HashCase> {};

TEST_P(BlockRoundtripContentHashMatrix, ContentHashBlobRoundTrips) {
    const HashCase& c = GetParam();
    CortrixBlock b = BaseBlock(0);
    b.content_hash = c.hash;
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.content_hash, c.hash) << c.label;
    EXPECT_EQ(g.content_hash.size(), c.hash.size()) << c.label;
}

static std::string BinHash(size_t n, unsigned char seed) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 17 + seed) & 0xFF);
    return s;
}

INSTANTIATE_TEST_SUITE_P(
    BlockContentHash, BlockRoundtripContentHashMatrix,
    ::testing::Values(
        HashCase{"", "empty_null"},
        HashCase{BinHash(16, 0), "bin16"},
        HashCase{BinHash(32, 7), "bin32"},
        HashCase{std::string("\x00\x01\x00\x02", 4), "embedded_nul"},
        HashCase{"plain-ascii-hash", "ascii"},
        HashCase{BinHash(8, 1), "bin8"},
        HashCase{BinHash(64, 3), "bin64"},
        HashCase{std::string(1, '\x00'), "single_nul"},
        HashCase{BinHash(20, 5), "bin20_sha1"}));

// ============================================================================
// (G) hnsw_node_id matrix: -1 (NULL) and assorted real ids round-trip.
// ============================================================================

class BlockRoundtripHnswMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<int64_t> {};

TEST_P(BlockRoundtripHnswMatrix, HnswNodeIdRoundTrips) {
    const int64_t nid = GetParam();
    CortrixBlock b = BaseBlock(0);
    b.hnsw_node_id = nid;
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.hnsw_node_id, nid) << "hnsw=" << nid;
}

INSTANTIATE_TEST_SUITE_P(
    BlockHnsw, BlockRoundtripHnswMatrix,
    ::testing::Values<int64_t>(-1, 0, 1, 42, 1000000, 9223372036854775807LL));

// ============================================================================
// (H) explicit block_id matrix: a caller-provided uint64 id is preserved (incl.
//     high-bit-set ids that store as negative INTEGER rowids).
// ============================================================================

class BlockRoundtripExplicitIdMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<uint64_t> {};

TEST_P(BlockRoundtripExplicitIdMatrix, ExplicitBlockIdPreserved) {
    const uint64_t id = GetParam();
    CortrixBlock b = BaseBlock(0, "explicit id payload");
    b.block_id = id;
    ASSERT_EQ(store_->block_insert(b), 0);
    EXPECT_EQ(b.block_id, id) << "id not overwritten by rowid";

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(id, g), 0);
    EXPECT_EQ(g.block_id, id);
    EXPECT_EQ(g.content_text, "explicit id payload");
}

INSTANTIATE_TEST_SUITE_P(
    BlockExplicitIds, BlockRoundtripExplicitIdMatrix,
    ::testing::Values<uint64_t>(1ULL, 2ULL, 1000ULL, 0xABCDEF0123456789ULL,
                                0x8000000000000001ULL, 0xFFFFFFFFFFFFFFFFULL));

// ============================================================================
// (I) block_get_by_doc ordering + count matrix: insert N blocks in reverse
//     chunk_index order; read back ascending; counts agree.
// ============================================================================

class BlockRoundtripByDocMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<int> {};

TEST_P(BlockRoundtripByDocMatrix, GetByDocOrderedAndCounted) {
    const int n = GetParam();
    for (int i = n - 1; i >= 0; --i) {
        CortrixBlock b = BaseBlock(i, "chunk " + std::to_string(i));
        ASSERT_EQ(store_->block_insert(b), 0);
    }

    std::vector<CortrixBlock> blocks;
    ASSERT_EQ(store_->block_get_by_doc(doc_id_, blocks), 0);
    ASSERT_EQ(static_cast<int>(blocks.size()), n);
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(blocks[i].chunk_index, i) << "n=" << n << " i=" << i;
    }

    int64_t bc = -1;
    ASSERT_EQ(store_->block_count_by_doc(doc_id_, &bc), 0);
    EXPECT_EQ(bc, n);

    int64_t total = -1;
    ASSERT_EQ(store_->block_count(&total), 0);
    EXPECT_EQ(total, n);
}

INSTANTIATE_TEST_SUITE_P(BlockByDoc, BlockRoundtripByDocMatrix,
                         ::testing::Values(0, 1, 2, 3, 5, 8, 16, 32));

// ============================================================================
// (J) block_count_by_doc isolation matrix: two docs with independent block
//     counts; each count is scoped to its own doc.
// ============================================================================

struct TwoDocCountCase {
    int blocks_a;
    int blocks_b;
    const char* label;
};

class BlockRoundtripCountByDocMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<TwoDocCountCase> {};

TEST_P(BlockRoundtripCountByDocMatrix, CountByDocIsScoped) {
    const TwoDocCountCase& c = GetParam();
    // doc_id_ (from fixture) is doc A; create a second doc B.
    CortrixDoc db;
    db.source_type = "blk_rt_owner_b";
    db.source_path = "owner/doc_b";
    ASSERT_EQ(store_->doc_create(db), 0);

    for (int i = 0; i < c.blocks_a; ++i) {
        CortrixBlock b = BaseBlock(i, "a" + std::to_string(i));
        ASSERT_EQ(store_->block_insert(b), 0);
    }
    for (int i = 0; i < c.blocks_b; ++i) {
        CortrixBlock b;
        b.doc_id = db.doc_id;
        b.chunk_index = i;
        b.block_type = kBlockFile;
        b.processing_level = kLevelL2;
        b.data = BlockBuild(kBlockFile, kLevelL2, "b" + std::to_string(i), "", "", 0, 0);
        b.content_text = "b" + std::to_string(i);
        ASSERT_EQ(store_->block_insert(b), 0);
    }

    int64_t ca = -1, cb = -1, total = -1;
    ASSERT_EQ(store_->block_count_by_doc(doc_id_, &ca), 0);
    ASSERT_EQ(store_->block_count_by_doc(db.doc_id, &cb), 0);
    ASSERT_EQ(store_->block_count(&total), 0);
    EXPECT_EQ(ca, c.blocks_a) << c.label;
    EXPECT_EQ(cb, c.blocks_b) << c.label;
    EXPECT_EQ(total, c.blocks_a + c.blocks_b) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    BlockCountByDoc, BlockRoundtripCountByDocMatrix,
    ::testing::Values(
        TwoDocCountCase{0, 0, "0_0"},
        TwoDocCountCase{3, 0, "3_0"},
        TwoDocCountCase{0, 4, "0_4"},
        TwoDocCountCase{5, 5, "5_5"},
        TwoDocCountCase{10, 2, "10_2"},
        TwoDocCountCase{7, 13, "7_13"}));

// ============================================================================
// (K) chunk_index matrix: assorted index values round-trip (incl. 0 and large).
// ============================================================================

class BlockRoundtripChunkIndexMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<int> {};

TEST_P(BlockRoundtripChunkIndexMatrix, ChunkIndexRoundTrips) {
    const int idx = GetParam();
    CortrixBlock b = BaseBlock(idx);
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.chunk_index, idx) << "chunk_index=" << idx;
}

INSTANTIATE_TEST_SUITE_P(BlockChunkIndex, BlockRoundtripChunkIndexMatrix,
                         ::testing::Values(0, 1, 7, 100, 255, 4096, 65535,
                                           1000000, 2147483647));

// ============================================================================
// (L) Full-block coexistence: many fields set at once survive together via both
//     block_get and block_get_by_doc.
// ============================================================================

TEST_F(BlockRoundtripFixture, FullChildBlockAllFieldsCoexist) {
    CortrixBlock b;
    b.block_id = 0xABCDEF0123456789ULL;
    b.doc_id = doc_id_;
    b.chunk_index = 3;
    b.block_type = kBlockImage;
    b.processing_level = kLevelL4;
    b.hnsw_node_id = 4242;
    b.content_hash = BinHash(32, 9);
    b.data = BlockBuild(kBlockImage, kLevelL4, kCjkContent, "", "", 0, 0);
    b.content_text = kCjkContent;
    b.child_id = "01HXCHILD000000000000FULL";
    b.parent_id = "01HXPARENT00000000000FULL";
    b.token_count = 777;
    b.parent_offset = 88;
    b.metadata_json = kCjkMetaJson;
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.block_id, b.block_id);
    EXPECT_EQ(g.doc_id, doc_id_);
    EXPECT_EQ(g.chunk_index, 3);
    EXPECT_EQ(g.block_type, kBlockImage);
    EXPECT_EQ(g.processing_level, kLevelL4);
    EXPECT_EQ(g.hnsw_node_id, 4242);
    EXPECT_EQ(g.content_hash, b.content_hash);
    EXPECT_EQ(g.content_text, kCjkContent);
    EXPECT_EQ(g.child_id, b.child_id);
    EXPECT_EQ(g.parent_id, b.parent_id);
    EXPECT_EQ(g.token_count, 777);
    EXPECT_EQ(g.parent_offset, 88);
    EXPECT_EQ(g.metadata_json, kCjkMetaJson);
    EXPECT_FALSE(g.data.empty());
}

// ============================================================================
// (M) PARENT table matrix: parent_insert/parent_get round-trip over the int and
//     text fields; metadata_json treated as opaque text.
// ============================================================================

// --- (M1) parent text-field matrix ---
enum class ParentTextField {
    kNamespaceId,
    kParentText,
    kMetadataJson,
};
struct ParentTextCase {
    ParentTextField field;
    std::string     value;
    const char*     label;
};

class ParentRoundtripTextMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<ParentTextCase> {};

TEST_P(ParentRoundtripTextMatrix, ParentTextFieldRoundTrips) {
    const ParentTextCase& c = GetParam();
    CortrixParent p;
    // parent_id must be unique per inserted row; derive it from the label index by
    // hashing the label into a stable suffix.
    p.parent_id = std::string("01HXP_") + c.label;
    p.doc_id = doc_id_;
    p.namespace_id = "ns_default";
    p.parent_text = "default parent text";
    p.token_count = 10;
    p.page_start = 0;
    p.page_end = 1;
    p.byte_offset_start = 0;
    p.byte_offset_end = 100;
    p.metadata_json = "{}";
    p.created_at = 1700000000000;

    switch (c.field) {
        case ParentTextField::kNamespaceId: p.namespace_id = c.value; break;
        case ParentTextField::kParentText:  p.parent_text = c.value; break;
        case ParentTextField::kMetadataJson:p.metadata_json = c.value; break;
    }
    ASSERT_EQ(store_->parent_insert(p), 0);

    CortrixParent g;
    ASSERT_EQ(store_->parent_get(p.parent_id, g), 0);
    switch (c.field) {
        case ParentTextField::kNamespaceId: EXPECT_EQ(g.namespace_id, c.value) << c.label; break;
        case ParentTextField::kParentText:  EXPECT_EQ(g.parent_text, c.value) << c.label; break;
        case ParentTextField::kMetadataJson:EXPECT_EQ(g.metadata_json, c.value) << c.label; break;
    }
}

INSTANTIATE_TEST_SUITE_P(
    ParentTextFields, ParentRoundtripTextMatrix,
    ::testing::Values(
        ParentTextCase{ParentTextField::kNamespaceId, "ns_alpha", "ns_alpha"},
        ParentTextCase{ParentTextField::kNamespaceId, "ns_with_underscores_123", "ns_long"},
        ParentTextCase{ParentTextField::kParentText, "A coarse ~1024-token span.", "ptext_ascii"},
        ParentTextCase{ParentTextField::kParentText, kCjkContent, "ptext_cjk"},
        ParentTextCase{ParentTextField::kParentText, std::string(32 * 1024, 'P'), "ptext_large32k"},
        ParentTextCase{ParentTextField::kParentText, "O'Reilly \"quote\"\nnl", "ptext_special"},
        ParentTextCase{ParentTextField::kMetadataJson, R"({"doc_title":"spec","doc_language":"en"})", "meta_flat"},
        ParentTextCase{ParentTextField::kMetadataJson, kCjkMetaJson, "meta_cjk"},
        ParentTextCase{ParentTextField::kMetadataJson, R"({"nested":{"a":[1,2]}})", "meta_nested"}));

// --- (M2) parent integer-field matrix ---
struct ParentIntCase {
    int64_t token_count;
    int64_t page_start;
    int64_t page_end;
    int64_t byte_offset_start;
    int64_t byte_offset_end;
    int64_t created_at;
    const char* label;
};

class ParentRoundtripIntMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<ParentIntCase> {};

TEST_P(ParentRoundtripIntMatrix, ParentIntFieldsRoundTrip) {
    const ParentIntCase& c = GetParam();
    CortrixParent p;
    p.parent_id = std::string("01HXPI_") + c.label;
    p.doc_id = doc_id_;
    p.namespace_id = "ns_int";
    p.parent_text = "int matrix parent";
    p.token_count = c.token_count;
    p.page_start = c.page_start;
    p.page_end = c.page_end;
    p.byte_offset_start = c.byte_offset_start;
    p.byte_offset_end = c.byte_offset_end;
    p.metadata_json = "{}";
    p.created_at = c.created_at;
    ASSERT_EQ(store_->parent_insert(p), 0);

    CortrixParent g;
    ASSERT_EQ(store_->parent_get(p.parent_id, g), 0);
    EXPECT_EQ(g.token_count, c.token_count) << c.label;
    EXPECT_EQ(g.page_start, c.page_start) << c.label;
    EXPECT_EQ(g.page_end, c.page_end) << c.label;
    EXPECT_EQ(g.byte_offset_start, c.byte_offset_start) << c.label;
    EXPECT_EQ(g.byte_offset_end, c.byte_offset_end) << c.label;
    EXPECT_EQ(g.created_at, c.created_at) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    ParentIntFields, ParentRoundtripIntMatrix,
    ::testing::Values(
        ParentIntCase{0, 0, 0, 0, 0, 0, "all_zero"},
        ParentIntCase{1024, 1, 2, 100, 5096, 1700000000000, "typical"},
        ParentIntCase{1, 1, 1, 0, 1, 1, "ones"},
        ParentIntCase{9223372036854775807LL, 1000, 2000, 0,
                      9223372036854775807LL, 9223372036854775807LL, "maxima"},
        ParentIntCase{512, 5, 5, 4096, 8192, 1234567890123LL, "same_page"},
        ParentIntCase{2048, 10, 25, 65536, 131072, 1700000000001, "big_offsets"}));

// --- (M3) parent not-found + duplicate-PK behavior ---
TEST_F(BlockRoundtripFixture, ParentNotFoundAndDuplicate) {
    CortrixParent missing;
    EXPECT_EQ(store_->parent_get("01HXNOPE00000000000000000", missing), -2);

    CortrixParent p;
    p.parent_id = "01HXDUP0000000000000000001";
    p.doc_id = doc_id_;
    p.namespace_id = "ns_dup";
    p.parent_text = "dup test";
    p.token_count = 1;
    p.metadata_json = "{}";
    p.created_at = 1700000000000;
    ASSERT_EQ(store_->parent_insert(p), 0);
    // Duplicate primary key -> -3 (already exists).
    EXPECT_EQ(store_->parent_insert(p), -3);
}

// ============================================================================
// (N) Full-block combination matrix: each case sets a realistic combination of
//     all CortrixBlock fields at once (type/level/hnsw/content_hash/content_text
//     + the four child sentinels + metadata_json) and asserts every field
//     survives both block_get and block_get_by_doc. Complements the single-field
//     matrices above by proving the columns coexist across many combinations.
// ============================================================================

struct FullBlockCase {
    int          block_type;
    int          processing_level;
    int64_t      hnsw_node_id;
    int          content_hash_len;   // 0 => empty/NULL; else binary hash of this length
    const char*  content_text;       // "" => NULL
    const char*  child_id;           // "" => NULL
    const char*  parent_id;          // "" => NULL
    int64_t      token_count;        // -1 => NULL
    int64_t      parent_offset;      // -1 => NULL
    const char*  metadata_json;      // "" => NULL
    const char*  label;
};

class BlockRoundtripFullMatrix
    : public BlockRoundtripFixture,
      public ::testing::WithParamInterface<FullBlockCase> {};

TEST_P(BlockRoundtripFullMatrix, AllFieldsCoexist) {
    const FullBlockCase& c = GetParam();
    const std::string hash =
        c.content_hash_len > 0 ? BinHash(c.content_hash_len, 3) : std::string();

    CortrixBlock b = BaseBlock(0, "full payload");
    b.block_type = c.block_type;
    b.processing_level = c.processing_level;
    b.hnsw_node_id = c.hnsw_node_id;
    b.content_hash = hash;
    b.content_text = c.content_text;
    b.child_id = c.child_id;
    b.parent_id = c.parent_id;
    b.token_count = c.token_count;
    b.parent_offset = c.parent_offset;
    b.metadata_json = c.metadata_json;
    ASSERT_EQ(store_->block_insert(b), 0);

    CortrixBlock g;
    ASSERT_EQ(store_->block_get(b.block_id, g), 0);
    EXPECT_EQ(g.block_type, c.block_type) << c.label;
    EXPECT_EQ(g.processing_level, c.processing_level) << c.label;
    EXPECT_EQ(g.hnsw_node_id, c.hnsw_node_id) << c.label;
    EXPECT_EQ(g.content_hash, hash) << c.label;
    EXPECT_EQ(g.content_text, c.content_text) << c.label;
    EXPECT_EQ(g.child_id, c.child_id) << c.label;
    EXPECT_EQ(g.parent_id, c.parent_id) << c.label;
    EXPECT_EQ(g.token_count, c.token_count) << c.label;
    EXPECT_EQ(g.parent_offset, c.parent_offset) << c.label;
    EXPECT_EQ(g.metadata_json, c.metadata_json) << c.label;

    std::vector<CortrixBlock> blocks;
    ASSERT_EQ(store_->block_get_by_doc(doc_id_, blocks), 0);
    ASSERT_EQ(blocks.size(), 1u) << c.label;
    EXPECT_EQ(blocks[0].block_type, c.block_type) << c.label;
    EXPECT_EQ(blocks[0].content_text, c.content_text) << c.label;
    EXPECT_EQ(blocks[0].child_id, c.child_id) << c.label;
    EXPECT_EQ(blocks[0].metadata_json, c.metadata_json) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    BlockFullStructs, BlockRoundtripFullMatrix,
    ::testing::Values(
        // plain L2 file block, no enrichment, no child columns
        FullBlockCase{kBlockFile, kLevelL2, -1, 0, "plain file block",
                      "", "", -1, -1, "", "file_l2_plain"},
        // L3 file child with all child columns
        FullBlockCase{kBlockFile, kLevelL3, 100, 16, "child content",
                      "01HXC0000000000000000001", "01HXP0000000000000000001",
                      42, 7, R"({"ner":["Acme"]})", "file_l3_child_full"},
        // OCR scan block, hnsw set, hash 32B, no children
        FullBlockCase{kBlockScan, kLevelL2, 5, 32, "ocr text",
                      "", "", -1, -1, R"({"ocr":"paddle"})", "scan_l2_hash32"},
        // CDC database block
        FullBlockCase{kBlockDatabase, kLevelL1, -1, 0, "",
                      "", "", -1, -1, "", "db_l1_null_content"},
        // audio block L4, child with zero counts (zero != NULL)
        FullBlockCase{kBlockAudio, kLevelL4, 7, 16, "transcript",
                      "01HXC0000000000000000002", "01HXP0000000000000000002",
                      0, 0, "{}", "audio_l4_zero_counts"},
        // video block, large token/offset, CJK metadata
        FullBlockCase{kBlockVideo, kLevelL3, 999, 16, "video caption",
                      "01HXC0000000000000000003", "01HXP0000000000000000003",
                      9223372036854775807LL, 2147483648LL, "{\"k\":\"v\"}",
                      "video_l3_large_ints"},
        // image block with CJK content_text
        FullBlockCase{kBlockImage, kLevelL4, 1, 16, "img caption",
                      "01HXC0000000000000000004", "", 256, -1, R"({"vision":true})",
                      "image_l4_partial_child"},
        // memory block, no hash, child id only
        FullBlockCase{kBlockMemory, kLevelL2, -1, 0, "memory note",
                      "01HXC0000000000000000005", "", -1, -1, "", "memory_child_id_only"},
        // meta block, summary-style
        FullBlockCase{kBlockMeta, kLevelL1, -1, 0, "doc metadata block",
                      "", "", -1, -1, R"({"doc_meta":true})", "meta_block"},
        // HyPE question block
        FullBlockCase{kBlockHypeQuestion, kLevelL3, 33, 16, "What is X?",
                      "01HXC0000000000000000006", "01HXP0000000000000000006",
                      12, 0, R"({"hype":true})", "hype_question"},
        // doc-summary block
        FullBlockCase{kBlockDocSummary, kLevelL4, -1, 32, "Summary of the doc.",
                      "", "", -1, -1, R"({"summary":true})", "doc_summary"},
        // file block with metadata-only enrichment, parent_id present only
        FullBlockCase{kBlockFile, kLevelL2, -1, 16, "another block",
                      "", "01HXP0000000000000000007", -1, 99, R"({"only":"parent+offset"})",
                      "file_parent_offset_only"}));

}  // namespace
