#include "cortrix/import/text_serializer.h"

#include <algorithm>

namespace cortrix::import {

std::string BuildSourceUri(const SourceContext& src, const std::string& row_id) {
    // postgres://host:port/db/table/<row_id> (the DB-import convention).
    return "postgres://" + src.host + ":" + std::to_string(src.port) + "/" + src.db_name +
           "/" + src.table + "/" + row_id;
}

std::string SourcePrefix(const SourceContext& src) {
    // Trailing '/' is load-bearing: the D3 overwrite LIKE '<prefix>%' must not let
    // table "users" match "users2" (§3.2 / R6 accidental-deletion mitigation).
    return "postgres://" + src.host + ":" + std::to_string(src.port) + "/" + src.db_name +
           "/" + src.table + "/";
}

std::string TextSerializer::RenderRow(const DbRow& row) {
    std::string out;
    for (const auto& [col, val] : row.columns) {
        out += col;
        out += ": ";
        if (!val.is_null) out += val.text;
        out += "\n";
    }
    return out;
}

nlohmann::json TextSerializer::BuildMetadata(const SourceContext& src,
                                             const std::string& row_id,
                                             int row_span,
                                             bool is_merge) {
    // The DB-import metadata_json block written into blocks. source_type = the import
    // discriminator the D3 overwrite + Agent provenance queries key off.
    nlohmann::json m;
    m["source"] = BuildSourceUri(src, row_id);
    m["source_type"] = "database_import";
    m["source_connection_ref"] = src.connection_ref;
    m["source_query_signature"] = src.query_signature;
    m["imported_at"] = src.imported_at_iso;
    m["imported_by"] = src.imported_by;
    m["import_task_id"] = src.import_task_id;
    if (is_merge) {
        // MERGE chunk: always record how many rows it folded (even a trailing 1-row
        // chunk is a merge product) so a reader can tell it is not a 1:1 row Block.
        m["merged_row_count"] = row_span;
    }
    return m;
}

std::vector<TextChunk> TextSerializer::Serialize(const std::vector<DbRow>& rows,
                                                 TextStrategy strategy,
                                                 const SourceContext& src,
                                                 int merge_size) {
    std::vector<TextChunk> chunks;
    if (rows.empty()) return chunks;

    if (strategy == TextStrategy::kPerRow) {
        chunks.reserve(rows.size());
        for (const DbRow& row : rows) {
            TextChunk chunk;
            chunk.text = RenderRow(row);
            chunk.source = BuildSourceUri(src, row.row_id);
            chunk.metadata = BuildMetadata(src, row.row_id, /*row_span=*/1, /*is_merge=*/false);
            chunks.push_back(std::move(chunk));
        }
        return chunks;
    }

    // MERGE: fold up to merge_size rows per chunk; the chunk keys off the first row's
    // id (the source URI / overwrite prefix is per-table, so any row's id anchors it).
    const int step = std::max(1, merge_size);
    for (size_t i = 0; i < rows.size(); i += static_cast<size_t>(step)) {
        size_t end = std::min(rows.size(), i + static_cast<size_t>(step));
        std::string text;
        for (size_t j = i; j < end; ++j) {
            if (j > i) text += "\n---\n";  // row separator inside a merged Block
            text += RenderRow(rows[j]);
        }
        TextChunk chunk;
        chunk.text = std::move(text);
        chunk.source = BuildSourceUri(src, rows[i].row_id);
        chunk.metadata =
            BuildMetadata(src, rows[i].row_id, static_cast<int>(end - i), /*is_merge=*/true);
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

}  // namespace cortrix::import
