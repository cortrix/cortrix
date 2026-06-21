// BREADTH store roundtrip matrices for CortrixStoreSqlite -- DOCUMENT side.
//
// Exhaustive field-value round-trip + edge coverage of the document CRUD surface
// (doc_create / doc_get / doc_update_status / doc_delete + cascade /
// doc_find_by_hash / doc_find_by_source / doc_count), beyond the hand-written
// cases in test_cortrix_store_sqlite.cpp. All matrices use a deterministic
// temp-file SQLite store opened via CortrixStoreSqlite::Open() (no network / LLM /
// clock dependency). CJK test DATA is injected via \x UTF-8 escapes so the source
// file stays pure ASCII; all comments are English.
//
// SUITE NAMING: every suite/fixture below is globally unique and prefixed
// `DocRoundtrip*` so it never collides with the existing StoreSqliteTest suite or
// any other test binary linked into the same gtest process.

#include <gtest/gtest.h>
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/common/block_header.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace cortrix;
namespace fs = std::filesystem;

namespace {

// UTF-8 bytes for CJK strings, written as \x escapes to keep the file ASCII.
// "\xe4\xb8\xad\xe6\x96\x87" == U+4E2D U+6587 ("Chinese").
const std::string kCjkTitle    = "\xe4\xb8\xad\xe6\x96\x87\xe6\xa0\x87\xe9\xa2\x98";  // title CJK
const std::string kCjkMetadata =
    "{\"k\":\"\xe5\x80\xbc\"}";  // {"k":"<CJK value>"} valid JSON w/ CJK value

// A reusable fixture: fresh temp-file store per test, fully owns its connection.
class DocRoundtripFixture : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cortrix_doc_rt_" + std::to_string(::testing::UnitTest::GetInstance()
                                                        ->random_seed()) +
                "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir_);
        db_path_ = (dir_ / "doc_rt.db").string();
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);
    }
    void TearDown() override {
        if (store_) store_->Close();
        store_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    // Helper: create a doc, assert success, return its minted id.
    std::string Create(CortrixDoc& d) {
        EXPECT_EQ(store_->doc_create(d), 0);
        EXPECT_FALSE(d.doc_id.empty());
        return d.doc_id;
    }
    fs::path dir_;
    std::string db_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
};

// ============================================================================
// (A) doc_create/doc_get round-trip over a SCALAR field-value matrix.
//     One parameterized case per (field, value) -- each independently asserts
//     the value survives the create->get round-trip.
// ============================================================================

// Which text field of CortrixDoc a case targets.
enum class DocTextField {
    kSourceType,
    kSourcePath,
    kSourceRef,
    kContentHash,
    kMimeType,
    kChunkStrategy,
    kTitle,
    kLanguage,
    kMetadataJson,
    kErrorMessage,   // set via doc_update_status, asserted separately below in (D)
    kProcessedAt,
    kCdcSchema,
    kCdcPosition,
    kCdcLastSync,
};

struct DocTextCase {
    DocTextField field;
    std::string  value;
    const char*  label;
};

class DocRoundtripTextMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<DocTextCase> {};

// Sets `value` into the targeted field of `d`.
static void ApplyDocText(CortrixDoc& d, DocTextField f, const std::string& v) {
    switch (f) {
        case DocTextField::kSourceType:    d.source_type = v; break;
        case DocTextField::kSourcePath:    d.source_path = v; break;
        case DocTextField::kSourceRef:     d.source_ref = v; break;
        case DocTextField::kContentHash:   d.content_hash = v; break;
        case DocTextField::kMimeType:      d.mime_type = v; break;
        case DocTextField::kChunkStrategy: d.chunk_strategy = v; break;
        case DocTextField::kTitle:         d.title = v; break;
        case DocTextField::kLanguage:      d.language = v; break;
        case DocTextField::kMetadataJson:  d.metadata_json = v; break;
        case DocTextField::kErrorMessage:  d.error_message = v; break;
        case DocTextField::kProcessedAt:   d.processed_at = v; break;
        case DocTextField::kCdcSchema:     d.cdc_schema = v; break;
        case DocTextField::kCdcPosition:   d.cdc_position = v; break;
        case DocTextField::kCdcLastSync:   d.cdc_last_sync = v; break;
    }
}
// Reads the targeted field back out of `d`.
static std::string ReadDocText(const CortrixDoc& d, DocTextField f) {
    switch (f) {
        case DocTextField::kSourceType:    return d.source_type;
        case DocTextField::kSourcePath:    return d.source_path;
        case DocTextField::kSourceRef:     return d.source_ref;
        case DocTextField::kContentHash:   return d.content_hash;
        case DocTextField::kMimeType:      return d.mime_type;
        case DocTextField::kChunkStrategy: return d.chunk_strategy;
        case DocTextField::kTitle:         return d.title;
        case DocTextField::kLanguage:      return d.language;
        case DocTextField::kMetadataJson:  return d.metadata_json;
        case DocTextField::kErrorMessage:  return d.error_message;
        case DocTextField::kProcessedAt:   return d.processed_at;
        case DocTextField::kCdcSchema:     return d.cdc_schema;
        case DocTextField::kCdcPosition:   return d.cdc_position;
        case DocTextField::kCdcLastSync:   return d.cdc_last_sync;
    }
    return "";
}

TEST_P(DocRoundtripTextMatrix, FieldValueSurvivesCreateGet) {
    const DocTextCase& c = GetParam();
    CortrixDoc d;
    // source_type/source_path always need a sane base so find_by_source has a key;
    // for cases that target those very fields the case value overrides the base.
    d.source_type = "rt_base_type";
    d.source_path = "rt/base/path";
    ApplyDocText(d, c.field, c.value);

    const std::string id = Create(d);

    CortrixDoc got;
    ASSERT_EQ(store_->doc_get(id, got), 0);
    EXPECT_EQ(ReadDocText(got, c.field), c.value)
        << "field=" << c.label;
}

// A large (~64KB) string value to exercise big-text binding paths.
static std::string BigString() { return std::string(64 * 1024, 'Z'); }

INSTANTIATE_TEST_SUITE_P(
    DocTextFields, DocRoundtripTextMatrix,
    ::testing::Values(
        // ---- source_type ----
        DocTextCase{DocTextField::kSourceType, "http_upload", "source_type/http"},
        DocTextCase{DocTextField::kSourceType, "fuse", "source_type/fuse"},
        DocTextCase{DocTextField::kSourceType, "cdc", "source_type/cdc"},
        DocTextCase{DocTextField::kSourceType, "db_import", "source_type/db_import"},
        DocTextCase{DocTextField::kSourceType, "memory", "source_type/memory"},
        // ---- source_path (incl. special chars) ----
        DocTextCase{DocTextField::kSourcePath, "/uploads/a.pdf", "source_path/simple"},
        DocTextCase{DocTextField::kSourcePath, "pg://h/db/public.users", "source_path/uri"},
        DocTextCase{DocTextField::kSourcePath, "O'Reilly's \"file\".txt", "source_path/quotes"},
        DocTextCase{DocTextField::kSourcePath, "path/with\nnewline", "source_path/newline"},
        DocTextCase{DocTextField::kSourcePath, BigString(), "source_path/large64k"},
        // ---- source_ref ----
        DocTextCase{DocTextField::kSourceRef, "user-123", "source_ref/simple"},
        DocTextCase{DocTextField::kSourceRef, "public.customers", "source_ref/table"},
        DocTextCase{DocTextField::kSourceRef, "", "source_ref/empty"},
        // ---- content_hash (text form here; BLOB variant covered for blocks) ----
        DocTextCase{DocTextField::kContentHash, "abc123def456", "content_hash/hex"},
        DocTextCase{DocTextField::kContentHash, "", "content_hash/empty"},
        // ---- mime_type ----
        DocTextCase{DocTextField::kMimeType, "application/pdf", "mime/pdf"},
        DocTextCase{DocTextField::kMimeType, "text/markdown; charset=utf-8", "mime/md"},
        DocTextCase{DocTextField::kMimeType, "image/png", "mime/png"},
        DocTextCase{DocTextField::kMimeType, "", "mime/empty"},
        // ---- chunk_strategy ----
        DocTextCase{DocTextField::kChunkStrategy, "recursive_split", "chunk/recursive"},
        DocTextCase{DocTextField::kChunkStrategy, "semantic", "chunk/semantic"},
        DocTextCase{DocTextField::kChunkStrategy, "", "chunk/empty"},
        // ---- title (incl. CJK via escape + large) ----
        DocTextCase{DocTextField::kTitle, "Test Document", "title/ascii"},
        DocTextCase{DocTextField::kTitle, kCjkTitle, "title/cjk"},
        DocTextCase{DocTextField::kTitle, "", "title/empty"},
        DocTextCase{DocTextField::kTitle, BigString(), "title/large64k"},
        DocTextCase{DocTextField::kTitle, "tab\tand\nnl", "title/whitespace"},
        // ---- language ----
        DocTextCase{DocTextField::kLanguage, "en", "lang/en"},
        DocTextCase{DocTextField::kLanguage, "zh", "lang/zh"},
        DocTextCase{DocTextField::kLanguage, "zh-Hant", "lang/zh-hant"},
        DocTextCase{DocTextField::kLanguage, "", "lang/empty"},
        // ---- metadata_json (incl. CJK value + nested + array + empty) ----
        DocTextCase{DocTextField::kMetadataJson, R"({"key":"value"})", "meta/flat"},
        DocTextCase{DocTextField::kMetadataJson, R"({"a":{"b":[1,2,3]}})", "meta/nested"},
        DocTextCase{DocTextField::kMetadataJson, R"({"arr":["x","y"]})", "meta/array"},
        DocTextCase{DocTextField::kMetadataJson, kCjkMetadata, "meta/cjk"},
        DocTextCase{DocTextField::kMetadataJson, "{}", "meta/emptyobj"},
        DocTextCase{DocTextField::kMetadataJson, "", "meta/empty"},
        // ---- processed_at ----
        DocTextCase{DocTextField::kProcessedAt, "2026-02-15T12:00:00.000Z", "processed_at/iso"},
        DocTextCase{DocTextField::kProcessedAt, "", "processed_at/empty"},
        // ---- CDC fields ----
        DocTextCase{DocTextField::kCdcSchema, R"({"columns":["id","name"]})", "cdc_schema/json"},
        DocTextCase{DocTextField::kCdcSchema, "", "cdc_schema/empty"},
        DocTextCase{DocTextField::kCdcPosition, "0/16B3748", "cdc_pos/lsn"},
        DocTextCase{DocTextField::kCdcPosition, "binlog.000042:12345", "cdc_pos/binlog"},
        DocTextCase{DocTextField::kCdcPosition, "", "cdc_pos/empty"},
        DocTextCase{DocTextField::kCdcLastSync, "2026-02-15T10:30:00Z", "cdc_sync/iso"},
        DocTextCase{DocTextField::kCdcLastSync, "", "cdc_sync/empty"},
        // ---- extra source_type values ----
        DocTextCase{DocTextField::kSourceType, "s3", "source_type/s3"},
        DocTextCase{DocTextField::kSourceType, "gdrive", "source_type/gdrive"},
        DocTextCase{DocTextField::kSourceType, "api", "source_type/api"},
        // ---- extra source_path edge values ----
        DocTextCase{DocTextField::kSourcePath, "path with spaces.txt", "source_path/spaces"},
        DocTextCase{DocTextField::kSourcePath, "C:\\win\\path\\file.doc", "source_path/backslash"},
        DocTextCase{DocTextField::kSourcePath, "x", "source_path/single_char"},
        DocTextCase{DocTextField::kSourcePath, "tab\there", "source_path/tab"},
        // ---- extra mime values ----
        DocTextCase{DocTextField::kMimeType, "application/json", "mime/json"},
        DocTextCase{DocTextField::kMimeType, "text/csv", "mime/csv"},
        DocTextCase{DocTextField::kMimeType, "audio/mpeg", "mime/mp3"},
        DocTextCase{DocTextField::kMimeType, "video/mp4", "mime/mp4"},
        // ---- extra title values ----
        DocTextCase{DocTextField::kTitle, "Title: with colon & ampersand", "title/punct"},
        DocTextCase{DocTextField::kTitle, "a", "title/single_char"},
        // ---- extra content_hash values ----
        DocTextCase{DocTextField::kContentHash, "sha256:0011223344556677", "content_hash/prefixed"},
        DocTextCase{DocTextField::kContentHash, "x", "content_hash/single"},
        // ---- extra language values ----
        DocTextCase{DocTextField::kLanguage, "fr", "lang/fr"},
        DocTextCase{DocTextField::kLanguage, "ja", "lang/ja"},
        DocTextCase{DocTextField::kLanguage, "und", "lang/und"},
        // ---- extra metadata_json values ----
        DocTextCase{DocTextField::kMetadataJson, R"({"num":3.14,"bool":true,"null":null})", "meta/types"},
        DocTextCase{DocTextField::kMetadataJson, R"([1,2,3])", "meta/toparray"},
        DocTextCase{DocTextField::kMetadataJson, R"({"deep":{"a":{"b":{"c":1}}}})", "meta/deep"},
        // ---- extra cdc_position values ----
        DocTextCase{DocTextField::kCdcPosition, "GTID:3E11FA47-1234", "cdc_pos/gtid"},
        DocTextCase{DocTextField::kCdcPosition, "0", "cdc_pos/zero"},
        // ---- extra source_ref values ----
        DocTextCase{DocTextField::kSourceRef, "schema.table.column", "source_ref/dotted"},
        DocTextCase{DocTextField::kSourceRef, kCjkTitle, "source_ref/cjk"},
        // ---- extra chunk_strategy values ----
        DocTextCase{DocTextField::kChunkStrategy, "fixed_size", "chunk/fixed"},
        DocTextCase{DocTextField::kChunkStrategy, "markdown_aware", "chunk/markdown"}));

// ============================================================================
// (B) doc_create/doc_get round-trip over the INTEGER fields (file_size,
//     processing_level) -- including 0, large, and boundary values.
// ============================================================================

struct DocIntCase {
    int64_t file_size;
    int     processing_level;
    const char* label;
};

class DocRoundtripIntMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<DocIntCase> {};

TEST_P(DocRoundtripIntMatrix, IntFieldsSurviveCreateGet) {
    const DocIntCase& c = GetParam();
    CortrixDoc d;
    d.source_type = "int_matrix";
    d.source_path = std::string("p/") + c.label;
    d.file_size = c.file_size;
    d.processing_level = c.processing_level;

    const std::string id = Create(d);
    CortrixDoc got;
    ASSERT_EQ(store_->doc_get(id, got), 0);
    EXPECT_EQ(got.file_size, c.file_size) << c.label;
    EXPECT_EQ(got.processing_level, c.processing_level) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    DocIntFields, DocRoundtripIntMatrix,
    ::testing::Values(
        DocIntCase{0, 0, "size0_lvl0"},
        DocIntCase{1, 1, "size1_lvl1"},
        DocIntCase{1024, 2, "size1k_lvl2"},
        DocIntCase{1048576, 3, "size1m_lvl3"},
        DocIntCase{4, 4, "size4_lvl4"},
        DocIntCase{9223372036854775807LL, 3, "sizemax_lvl3"},
        DocIntCase{2147483648LL, 3, "size2g_lvl3"},
        DocIntCase{500, 0, "size500_lvl0"}));

// ============================================================================
// (C) doc_create/doc_get round-trip over the full DocStatus enum domain -- the
//     initial create-time status. (Update transitions are matrix (D).)
// ============================================================================

class DocRoundtripCreateStatusMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<DocStatus> {};

TEST_P(DocRoundtripCreateStatusMatrix, CreateTimeStatusRoundTrips) {
    const DocStatus s = GetParam();
    CortrixDoc d;
    d.source_type = "status_create";
    d.source_path = std::string("p/") + std::to_string(static_cast<int>(s));
    d.status = s;

    const std::string id = Create(d);
    CortrixDoc got;
    ASSERT_EQ(store_->doc_get(id, got), 0);
    // kDeleted is the soft-delete sentinel; create stores it like any other
    // in-range status. Every defined value round-trips to itself.
    EXPECT_EQ(got.status, s) << "status=" << static_cast<int>(s);
}

INSTANTIATE_TEST_SUITE_P(
    DocCreateStatuses, DocRoundtripCreateStatusMatrix,
    ::testing::Values(DocStatus::kPending, DocStatus::kProcessing,
                      DocStatus::kReady, DocStatus::kError, DocStatus::kStale,
                      DocStatus::kDeleting, DocStatus::kDeleted));

// ============================================================================
// (D) doc_update_status transition matrix: from every status to every status,
//     asserting the read-back status (and error_message when targeting kError).
// ============================================================================

struct DocStatusTransition {
    DocStatus from;
    DocStatus to;
};

class DocRoundtripStatusTransitionMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<DocStatusTransition> {};

TEST_P(DocRoundtripStatusTransitionMatrix, TransitionPersists) {
    const DocStatusTransition t = GetParam();
    CortrixDoc d;
    d.source_type = "transition";
    d.source_path = "p/" + std::to_string(static_cast<int>(t.from)) + "_" +
                    std::to_string(static_cast<int>(t.to));
    d.status = t.from;
    const std::string id = Create(d);

    // Sanity: the create-time status landed.
    CortrixDoc mid;
    ASSERT_EQ(store_->doc_get(id, mid), 0);
    ASSERT_EQ(mid.status, t.from);

    const std::string msg = (t.to == DocStatus::kError) ? "boom: parse failed" : "";
    ASSERT_EQ(store_->doc_update_status(id, t.to, msg), 0);

    CortrixDoc got;
    ASSERT_EQ(store_->doc_get(id, got), 0);
    EXPECT_EQ(got.status, t.to)
        << "from=" << static_cast<int>(t.from) << " to=" << static_cast<int>(t.to);
    if (t.to == DocStatus::kError) {
        EXPECT_EQ(got.error_message, msg);
    }
}

// Build the full Cartesian product of the 7 defined statuses (49 transitions).
static std::vector<DocStatusTransition> AllStatusTransitions() {
    const DocStatus all[] = {DocStatus::kPending, DocStatus::kProcessing,
                             DocStatus::kReady,   DocStatus::kError,
                             DocStatus::kStale,   DocStatus::kDeleting,
                             DocStatus::kDeleted};
    std::vector<DocStatusTransition> v;
    for (DocStatus a : all)
        for (DocStatus b : all) v.push_back({a, b});
    return v;
}

INSTANTIATE_TEST_SUITE_P(DocStatusTransitions,
                         DocRoundtripStatusTransitionMatrix,
                         ::testing::ValuesIn(AllStatusTransitions()));

// ============================================================================
// (E) doc_find_by_hash matrix: distinct hashes resolve to the right doc; empty
//     and absent hashes return -2 (not found).
// ============================================================================

struct FindByHashCase {
    std::string hash;     // the hash stored on the target doc
    bool        expect_found;
    const char* label;
};

class DocRoundtripFindByHashMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<FindByHashCase> {};

TEST_P(DocRoundtripFindByHashMatrix, FindByHash) {
    const FindByHashCase& c = GetParam();

    // Seed several decoy docs with distinct hashes so the matcher must discriminate.
    for (int i = 0; i < 5; ++i) {
        CortrixDoc decoy;
        decoy.source_type = "hash_decoy";
        decoy.source_path = "decoy/" + std::to_string(i);
        decoy.content_hash = "decoy_hash_" + std::to_string(i);
        Create(decoy);
    }

    std::string target_id;
    if (c.expect_found) {
        CortrixDoc target;
        target.source_type = "hash_target";
        target.source_path = std::string("target/") + c.label;
        target.content_hash = c.hash;
        target_id = Create(target);
    }

    CortrixDoc found;
    int ret = store_->doc_find_by_hash(c.hash, found);
    if (c.expect_found) {
        EXPECT_EQ(ret, 0) << c.label;
        EXPECT_EQ(found.doc_id, target_id) << c.label;
        EXPECT_EQ(found.content_hash, c.hash) << c.label;
    } else {
        EXPECT_EQ(ret, -2) << c.label;
    }
}

INSTANTIATE_TEST_SUITE_P(
    DocFindByHash, DocRoundtripFindByHashMatrix,
    ::testing::Values(
        FindByHashCase{"sha256:deadbeef", true, "found/prefixed"},
        FindByHashCase{"0123456789abcdef", true, "found/hex"},
        FindByHashCase{"unique-hash-xyz", true, "found/text"},
        FindByHashCase{"", false, "notfound/empty"},
        FindByHashCase{"does_not_exist_hash", false, "notfound/absent"}));

// ============================================================================
// (F) doc_find_by_source matrix: (source_type, source_path) pairs resolve to the
//     right doc or -2; verifies CDC fields are preserved through the lookup.
// ============================================================================

struct FindBySourceCase {
    std::string query_type;
    std::string query_path;
    bool        expect_found;
    const char* label;
};

class DocRoundtripFindBySourceMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<FindBySourceCase> {};

TEST_P(DocRoundtripFindBySourceMatrix, FindBySource) {
    const FindBySourceCase& c = GetParam();

    // Seed a fixed set of (type, path) docs; one of them is a CDC doc.
    struct Seed { const char* type; const char* path; bool cdc; };
    const Seed seeds[] = {
        {"http_upload", "/uploads/a.pdf", false},
        {"http_upload", "/uploads/b.pdf", false},
        {"fuse", "/mnt/data/c.txt", false},
        {"cdc", "pg://h/db/schema.users", true},
    };
    std::string cdc_id;
    for (const auto& s : seeds) {
        CortrixDoc d;
        d.source_type = s.type;
        d.source_path = s.path;
        if (s.cdc) {
            d.cdc_position = "binlog-pos-42";
            d.cdc_schema = R"({"cols":["a","b"]})";
        }
        std::string id = Create(d);
        if (s.cdc) cdc_id = id;
    }

    CortrixDoc found;
    int ret = store_->doc_find_by_source(c.query_type, c.query_path, found);
    if (c.expect_found) {
        EXPECT_EQ(ret, 0) << c.label;
        EXPECT_EQ(found.source_type, c.query_type) << c.label;
        EXPECT_EQ(found.source_path, c.query_path) << c.label;
        if (c.query_type == "cdc") {
            EXPECT_EQ(found.doc_id, cdc_id) << c.label;
            EXPECT_EQ(found.cdc_position, "binlog-pos-42") << c.label;
            EXPECT_EQ(found.cdc_schema, R"({"cols":["a","b"]})") << c.label;
        }
    } else {
        EXPECT_EQ(ret, -2) << c.label;
    }
}

INSTANTIATE_TEST_SUITE_P(
    DocFindBySource, DocRoundtripFindBySourceMatrix,
    ::testing::Values(
        FindBySourceCase{"http_upload", "/uploads/a.pdf", true, "found/a"},
        FindBySourceCase{"http_upload", "/uploads/b.pdf", true, "found/b"},
        FindBySourceCase{"fuse", "/mnt/data/c.txt", true, "found/fuse"},
        FindBySourceCase{"cdc", "pg://h/db/schema.users", true, "found/cdc"},
        // right path, wrong type -> no match (both columns must match)
        FindBySourceCase{"fuse", "/uploads/a.pdf", false, "notfound/type_mismatch"},
        // right type, wrong path
        FindBySourceCase{"http_upload", "/uploads/zzz.pdf", false, "notfound/path"},
        FindBySourceCase{"nope", "nope", false, "notfound/both"}));

// ============================================================================
// (G) doc_count matrix: after N creates then M deletes the count == N-M.
// ============================================================================

struct DocCountCase {
    int creates;
    int deletes;     // delete the first `deletes` created docs
    const char* label;
};

class DocRoundtripCountMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<DocCountCase> {};

TEST_P(DocRoundtripCountMatrix, CountAfterCreatesAndDeletes) {
    const DocCountCase& c = GetParam();
    int64_t count = -1;
    ASSERT_EQ(store_->doc_count(&count), 0);
    ASSERT_EQ(count, 0);

    std::vector<std::string> ids;
    for (int i = 0; i < c.creates; ++i) {
        CortrixDoc d;
        d.source_type = "count";
        d.source_path = "p/" + std::to_string(i);
        ids.push_back(Create(d));
    }
    ASSERT_EQ(store_->doc_count(&count), 0);
    EXPECT_EQ(count, c.creates) << c.label;

    for (int i = 0; i < c.deletes && i < static_cast<int>(ids.size()); ++i) {
        EXPECT_EQ(store_->doc_delete(ids[i]), 0) << c.label;
    }
    ASSERT_EQ(store_->doc_count(&count), 0);
    EXPECT_EQ(count, c.creates - c.deletes) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    DocCounts, DocRoundtripCountMatrix,
    ::testing::Values(
        DocCountCase{0, 0, "empty"},
        DocCountCase{1, 0, "one"},
        DocCountCase{1, 1, "one_del_one"},
        DocCountCase{5, 0, "five"},
        DocCountCase{5, 2, "five_del_two"},
        DocCountCase{5, 5, "five_del_all"},
        DocCountCase{20, 7, "twenty_del_seven"},
        DocCountCase{50, 25, "fifty_del_half"}));

// ============================================================================
// (H) doc_delete + cascade matrix: a doc with B blocks; after delete the doc is
//     gone (-2) and its blocks are gone too. Parameterized over block count.
// ============================================================================

class DocRoundtripDeleteCascadeMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<int> {};

TEST_P(DocRoundtripDeleteCascadeMatrix, DeleteCascadesToBlocks) {
    const int n_blocks = GetParam();
    CortrixDoc d;
    d.source_type = "cascade";
    d.source_path = "p/" + std::to_string(n_blocks);
    const std::string id = Create(d);

    for (int i = 0; i < n_blocks; ++i) {
        CortrixBlock b;
        b.doc_id = id;
        b.chunk_index = i;
        b.block_type = kBlockFile;
        b.processing_level = kLevelL2;
        // data is NOT NULL -- always bind a non-empty payload.
        b.data = BlockBuild(kBlockFile, kLevelL2, "row " + std::to_string(i),
                            "", "", 0, 0);
        b.content_text = "row " + std::to_string(i);
        ASSERT_EQ(store_->block_insert(b), 0);
    }

    int64_t bc = -1;
    ASSERT_EQ(store_->block_count_by_doc(id, &bc), 0);
    ASSERT_EQ(bc, n_blocks);

    ASSERT_EQ(store_->doc_delete(id), 0);

    CortrixDoc gone;
    EXPECT_EQ(store_->doc_get(id, gone), -2);

    std::vector<CortrixBlock> blocks;
    EXPECT_EQ(store_->block_get_by_doc(id, blocks), 0);
    EXPECT_TRUE(blocks.empty());

    ASSERT_EQ(store_->block_count_by_doc(id, &bc), 0);
    EXPECT_EQ(bc, 0);
}

INSTANTIATE_TEST_SUITE_P(DocDeleteCascade,
                         DocRoundtripDeleteCascadeMatrix,
                         ::testing::Values(0, 1, 2, 3, 5, 10, 25));

// ============================================================================
// (I) A few hand-written full-struct round-trips combining many fields at once
//     (the matrices above isolate one field; these prove they all coexist).
// ============================================================================

TEST_F(DocRoundtripFixture, FullCdcDocAllFieldsCoexist) {
    CortrixDoc d;
    d.source_type = "cdc";
    d.source_path = "pg://localhost/mydb/public.customers";
    d.source_ref = "public.customers";
    d.content_hash = "hash-coexist";
    d.file_size = 987654321LL;
    d.mime_type = "application/x-cdc";
    d.status = DocStatus::kReady;
    d.processing_level = 4;
    d.chunk_strategy = "semantic";
    d.processed_at = "2026-02-15T12:00:00.000Z";
    d.cdc_schema = R"({"columns":["id","name","email"]})";
    d.cdc_position = "0/16B3748";
    d.cdc_last_sync = "2026-02-15T10:30:00Z";
    d.title = kCjkTitle;
    d.language = "zh";
    d.metadata_json = kCjkMetadata;

    const std::string id = Create(d);
    CortrixDoc g;
    ASSERT_EQ(store_->doc_get(id, g), 0);
    EXPECT_EQ(g.source_type, d.source_type);
    EXPECT_EQ(g.source_path, d.source_path);
    EXPECT_EQ(g.source_ref, d.source_ref);
    EXPECT_EQ(g.content_hash, d.content_hash);
    EXPECT_EQ(g.file_size, d.file_size);
    EXPECT_EQ(g.mime_type, d.mime_type);
    EXPECT_EQ(g.status, DocStatus::kReady);
    EXPECT_EQ(g.processing_level, 4);
    EXPECT_EQ(g.chunk_strategy, d.chunk_strategy);
    EXPECT_EQ(g.processed_at, d.processed_at);
    EXPECT_EQ(g.cdc_schema, d.cdc_schema);
    EXPECT_EQ(g.cdc_position, d.cdc_position);
    EXPECT_EQ(g.cdc_last_sync, d.cdc_last_sync);
    EXPECT_EQ(g.title, d.title);
    EXPECT_EQ(g.language, d.language);
    EXPECT_EQ(g.metadata_json, d.metadata_json);
}

TEST_F(DocRoundtripFixture, MinimalDocOnlyRequiredKeys) {
    // Only source_type + source_path; everything else empty/default.
    CortrixDoc d;
    d.source_type = "minimal";
    d.source_path = "min/path";
    const std::string id = Create(d);

    CortrixDoc g;
    ASSERT_EQ(store_->doc_get(id, g), 0);
    EXPECT_EQ(g.status, DocStatus::kPending);
    EXPECT_EQ(g.file_size, 0);
    EXPECT_TRUE(g.content_hash.empty());
    EXPECT_TRUE(g.title.empty());
    EXPECT_TRUE(g.cdc_position.empty());
    EXPECT_FALSE(g.created_at.empty());
}

// ============================================================================
// (J) Full-struct round-trip matrix: each case sets a realistic combination of
//     all CortrixDoc fields at once, and asserts every field survives create+get.
//     This complements the single-field matrices (A-C) by proving fields coexist
//     across many combinations. Each row is one parameterized case.
// ============================================================================

struct FullDocCase {
    const char* source_type;
    const char* source_path;
    const char* source_ref;
    const char* content_hash;
    int64_t     file_size;
    const char* mime_type;
    DocStatus   status;
    int         processing_level;
    const char* chunk_strategy;
    const char* processed_at;
    const char* cdc_schema;
    const char* cdc_position;
    const char* cdc_last_sync;
    const char* title;
    const char* language;
    const char* metadata_json;
    const char* label;
};

class DocRoundtripFullStructMatrix
    : public DocRoundtripFixture,
      public ::testing::WithParamInterface<FullDocCase> {};

TEST_P(DocRoundtripFullStructMatrix, AllFieldsCoexist) {
    const FullDocCase& c = GetParam();
    CortrixDoc d;
    d.source_type = c.source_type;
    d.source_path = c.source_path;
    d.source_ref = c.source_ref;
    d.content_hash = c.content_hash;
    d.file_size = c.file_size;
    d.mime_type = c.mime_type;
    d.status = c.status;
    d.processing_level = c.processing_level;
    d.chunk_strategy = c.chunk_strategy;
    d.processed_at = c.processed_at;
    d.cdc_schema = c.cdc_schema;
    d.cdc_position = c.cdc_position;
    d.cdc_last_sync = c.cdc_last_sync;
    d.title = c.title;
    d.language = c.language;
    d.metadata_json = c.metadata_json;
    const std::string id = Create(d);

    CortrixDoc g;
    ASSERT_EQ(store_->doc_get(id, g), 0);
    EXPECT_EQ(g.source_type, c.source_type) << c.label;
    EXPECT_EQ(g.source_path, c.source_path) << c.label;
    EXPECT_EQ(g.source_ref, c.source_ref) << c.label;
    EXPECT_EQ(g.content_hash, c.content_hash) << c.label;
    EXPECT_EQ(g.file_size, c.file_size) << c.label;
    EXPECT_EQ(g.mime_type, c.mime_type) << c.label;
    // Out-of-range status falls back to pending at the store layer; all cases
    // below use defined statuses so the round-trip is exact.
    EXPECT_EQ(g.status, c.status) << c.label;
    EXPECT_EQ(g.processing_level, c.processing_level) << c.label;
    EXPECT_EQ(g.chunk_strategy, c.chunk_strategy) << c.label;
    EXPECT_EQ(g.processed_at, c.processed_at) << c.label;
    EXPECT_EQ(g.cdc_schema, c.cdc_schema) << c.label;
    EXPECT_EQ(g.cdc_position, c.cdc_position) << c.label;
    EXPECT_EQ(g.cdc_last_sync, c.cdc_last_sync) << c.label;
    EXPECT_EQ(g.title, c.title) << c.label;
    EXPECT_EQ(g.language, c.language) << c.label;
    EXPECT_EQ(g.metadata_json, c.metadata_json) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    DocFullStructs, DocRoundtripFullStructMatrix,
    ::testing::Values(
        FullDocCase{"http_upload", "/u/1.pdf", "owner1", "h1", 100, "application/pdf",
                    DocStatus::kPending, 3, "recursive_split", "", "", "", "",
                    "Doc One", "en", R"({"k":1})", "http_pending"},
        FullDocCase{"http_upload", "/u/2.docx", "owner2", "h2", 2048,
                    "application/vnd.openxmlformats", DocStatus::kProcessing, 2,
                    "semantic", "2026-01-01T00:00:00Z", "", "", "", "Doc Two", "en",
                    R"({"k":2})", "http_processing"},
        FullDocCase{"fuse", "/mnt/a.txt", "", "", 0, "text/plain",
                    DocStatus::kReady, 1, "", "2026-01-02T00:00:00Z", "", "", "",
                    "", "", "", "fuse_ready_minimal"},
        FullDocCase{"fuse", "/mnt/big.bin", "", "hbig", 1073741824LL,
                    "application/octet-stream", DocStatus::kReady, 0, "", "", "",
                    "", "", "Big Binary", "en", "{}", "fuse_ready_1gb"},
        FullDocCase{"cdc", "pg://h/db/public.users", "public.users", "", 0,
                    "application/x-cdc", DocStatus::kReady, 4, "row_per_block",
                    "2026-01-03T00:00:00Z", R"({"columns":["id","name"]})",
                    "0/16B3748", "2026-01-03T00:00:00Z", "Users Table", "en",
                    R"({"cdc":true})", "cdc_full"},
        FullDocCase{"cdc", "mysql://h/db/orders", "orders", "", 0, "application/x-cdc",
                    DocStatus::kProcessing, 3, "", "", R"({"columns":["oid"]})",
                    "binlog.000001:42", "2026-01-04T00:00:00Z", "Orders", "en",
                    R"({"engine":"mysql"})", "cdc_mysql"},
        FullDocCase{"memory", "mem://session/42", "session42", "hmem", 64,
                    "application/json", DocStatus::kReady, 2, "", "", "", "", "",
                    "Memory Note", "en", R"({"role":"user"})", "memory_ready"},
        FullDocCase{"http_upload", "/u/err.pdf", "owner_err", "herr", 10,
                    "application/pdf", DocStatus::kError, 3, "recursive_split", "",
                    "", "", "", "Errored Doc", "en", "{}", "http_error"},
        FullDocCase{"fuse", "/mnt/stale.txt", "", "hstale", 5, "text/plain",
                    DocStatus::kStale, 1, "", "", "", "", "", "Stale", "en", "{}",
                    "fuse_stale"},
        FullDocCase{"http_upload", "/u/del.pdf", "owner_del", "hdel", 7,
                    "application/pdf", DocStatus::kDeleting, 3, "", "", "", "", "",
                    "Deleting", "en", "{}", "http_deleting"},
        FullDocCase{"http_upload", "/u/gone.pdf", "owner_gone", "hgone", 9,
                    "application/pdf", DocStatus::kDeleted, 3, "", "", "", "", "",
                    "Gone", "en", "{}", "http_deleted"},
        FullDocCase{"http_upload", "/u/cjk.pdf", "cjk_owner", "hcjk", 11,
                    "application/pdf", DocStatus::kReady, 3, "semantic", "", "", "",
                    "", "CJK", "zh", R"({"v":1})", "http_cjk_lang"},
        FullDocCase{"db_import", "postgres://h/db/t/1", "t", "", 0, "text/csv",
                    DocStatus::kReady, 2, "row_per_block", "", "", "", "",
                    "Imported Row", "en", R"({"row":1})", "db_import_row"},
        FullDocCase{"http_upload", "/u/maxlvl.pdf", "owner_max", "hmax",
                    9223372036854775807LL, "application/pdf", DocStatus::kReady, 4,
                    "recursive_split", "2026-06-01T00:00:00Z", "", "", "", "Max",
                    "en", R"({"max":true})", "http_max_size_lvl4"},
        FullDocCase{"fuse", "/mnt/empty_meta.txt", "", "", 0, "", DocStatus::kPending,
                    0, "", "", "", "", "", "", "", "", "fuse_all_empty_optional"}));

}  // namespace
