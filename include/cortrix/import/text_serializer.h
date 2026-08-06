#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/import/import_types.h"

namespace cortrix::import {

/// Everything needed to stamp a row's Block with its provenance. The
/// serializer fills blocks.metadata_json `source` (postgres://host:port/db/table/
/// <row_id>) + the source_type / connection_ref / query_signature / import_task_id /
/// imported_at / imported_by fields the full-overwrite + audit rely on.
struct SourceContext {
    std::string host;
    int port = 5432;
    std::string db_name;
    std::string table;                 ///< logical table name for the source URI
    ConnectionRefId connection_ref;
    ImportTaskId import_task_id;
    std::string query_signature;       ///< query signature (table+filter / sql hash) for overwrite matching
    std::string imported_by;           ///< user_id
    std::string imported_at_iso;       ///< RFC3339 UTC
};

/// The postgres:// source URI for a row. The full-
/// overwrite matches the prefix up to ".../table/" (see SourcePrefix).
std::string BuildSourceUri(const SourceContext& src, const std::string& row_id);

/// The source PREFIX for `src` (postgres://host:port/db/table/) — the LIKE anchor
/// the full-overwrite uses to clear only this table's prior Blocks (
/// cleanup_source_blocks /). A trailing '/' so "users" never matches "users2".
std::string SourcePrefix(const SourceContext& src);

/// textualization engine. Two strategies only — PER_ROW + MERGE; the
/// TEMPLATE/Jinja path was removed in v1.0.2 (V3 ruling 13 — no SSTI attack surface
/// in V1.0; Jinja / LLM textualization is re-evaluated at V1.5). No template, no
/// sandbox.
class TextSerializer {
public:
    /// Serialize fetched rows into Blocks-to-be. PER_ROW → one TextChunk per row;
    /// MERGE → ceil(N/merge_size) chunks, each concatenating up to merge_size rows.
    /// Every chunk carries its source URI + the metadata block.
    std::vector<TextChunk> Serialize(const std::vector<DbRow>& rows,
                                     TextStrategy strategy,
                                     const SourceContext& src,
                                     int merge_size = 10);

private:
    /// PER_ROW rendering: "col1: val1\ncol2: val2\n..." (null columns rendered as
    /// "col: " — present but empty, so the Block keeps the schema shape).
    std::string RenderRow(const DbRow& row);

    /// The metadata_json block for a chunk built from `first_row` (its row_id
    /// seeds the source URI). For MERGE chunks (`is_merge`), `merged_row_count` is
    /// always emitted (even a trailing 1-row chunk is a merge product, distinct from
    /// a PER_ROW Block); PER_ROW chunks omit it.
    nlohmann::json BuildMetadata(const SourceContext& src,
                                 const std::string& row_id,
                                 int row_span,
                                 bool is_merge);
};

}  // namespace cortrix::import
