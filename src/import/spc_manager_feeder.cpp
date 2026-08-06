#include <cstdint>
#include "cortrix/import/spc_manager_feeder.h"

#include <string>

#include "cortrix/id/ulid.h"
#include "cortrix/import/import_error.h"
#include "cortrix/query/content_hash.h"
#include "cortrix/resource/namespace_facade.h"   // [P2c] per-NS store for cleanup
#include "cortrix/spc/parser.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/spc_task.h"
#include "cortrix/store/cortrix_store.h"          // [P2c] doc_delete_by_source_prefix

namespace cortrix::import {

namespace {

// Build a one-page ParsedDoc from a textualized DB row. The row text becomes the
// single page's text + one TEXT paragraph (the chunker's input material); the
// source metadata rides on the SPCTask.metadata_json (set by the caller), not
// here. status=kOk so ProcessParsedDoc treats it as a normal post-parse doc.
spc::ParsedDoc MakeParsedDoc(const TextChunk& chunk) {
    spc::ParsedDoc doc;
    doc.status = spc::ParserErrorCode::kOk;
    doc.parser_name = "db_import";  // textualized DB source (no parser run)

    spc::ParsedChunk para;
    para.text = chunk.text;
    para.page = 1;
    para.type = spc::ChunkType::TEXT;
    para.confidence = 1.0f;

    spc::ParsedPage page;
    page.page_num = 1;
    page.page_text = chunk.text;
    page.paragraphs.push_back(std::move(para));
    page.page_metadata.page_num = 1;
    page.page_metadata.parser_used = "db_import";
    doc.pages.push_back(std::move(page));

    doc.metadata.mime_type = "text/plain";
    doc.metadata.page_count = 1;
    return doc;
}

}  // namespace

Result<int> SpcManagerFeeder::Feed(const std::vector<TextChunk>& chunks, const NsId& ns) {
    int ingested = 0;
    for (const auto& chunk : chunks) {
        spc::ParsedDoc parsed = MakeParsedDoc(chunk);

        SPCTask task;
        task.doc_id = id::GenerateUlid();
        task.namespace_name = ns;
        task.source_path = chunk.source;            // postgres://.../table/<row_id>
        task.source_type = "db_import";             // DB-import provenance (vs "file"/"http_upload")
        task.content_hash = query::ContentHashOfContent(chunk.text);
        task.mime_type = "text/plain";
        task.processing_level = 3;                  // L3 full processing (embed + store)
        // The source metadata (source_type/ref/signature/imported_*) flows into
        // the block records through SPCTask.metadata_json.
        task.metadata_json = chunk.metadata.dump();

        int rc = spc_mgr_->ProcessParsedDoc(parsed, task);
        if (rc != 0) {
            // Surface the failure with the DB-import connection-failed token (the safest
            // transient default for an internal ingest fault), carrying the pipeline
            // detail for the operator.
            return ImportStatus(ImportErrorCode::kConnectionFailed,
                              "SPC pipeline ingest failed for row '" + chunk.source +
                                  "': " + task.error_message);
        }
        ++ingested;
    }
    return ingested;
}

Result<int> RealBlockCleaner::CleanupSourceBlocks(const NsId& ns,
                                                  const std::string& source_prefix) {
    if (source_prefix.empty()) return 0;  // never clear a whole namespace
    // Acquire the same per-Unit façade the feeder writes into, then delete this
    // table's prior documents + blocks by source_path prefix (one txn in the store).
    resource::NamespaceFacade facade(*pool_, ns);
    Status acq = facade.Acquire();
    if (!acq.ok()) {
        // A namespace that does not exist yet = nothing to clear (first import).
        return 0;
    }
    int64_t removed = 0;
    int rc = facade.store().doc_delete_by_source_prefix(source_prefix, &removed);
    if (rc != 0) {
        return ImportStatus(ImportErrorCode::kConnectionFailed,
                          "failed to clear prior import blocks for prefix '" +
                              source_prefix + "'");
    }
    return static_cast<int>(removed);
}

Result<int> NoopBlockCleaner::CleanupSourceBlocks(const NsId& /*ns*/,
                                                  const std::string& /*source_prefix*/) {
    // Standalone/test double: records nothing, clears nothing. Production uses
    // RealBlockCleaner (real store DELETE-by-source). Never fails.
    return 0;
}

}  // namespace cortrix::import
