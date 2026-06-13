#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/id/types.h"

namespace cortrix::import {

/// Phase-1 string-alias IDs (ARCH §1.8 — distinct strong types are Phase 2). A
/// connection ref is "db_conn_<ulid>"; an import task is "import_<ulid>" (F16a
/// §4.1 / §4.2). NsId / TenantId reuse the catalog's TEXT keys (F12).
using ConnectionRefId = std::string;
using ImportTaskId    = std::string;
using NsId            = cortrix::id::NamespaceId;
using TenantId        = std::string;

/// D4 textualization strategy (F16a §3.5). TEMPLATE was removed in v1.0.2 (D1 V3
/// ruling 13 — Jinja pushed to V1.5); only PER_ROW + MERGE exist in V1.0.
enum class TextStrategy {
    kPerRow,   ///< default: one Block per row ("col: val\n...")
    kMerge,    ///< N rows concatenated into one Block
};

const char* ToString(TextStrategy strategy);
/// Parse the wire enum ("per_row" | "merge"). Returns nullopt for anything else
/// (notably "template", deliberately rejected in V1.0).
std::optional<TextStrategy> ParseTextStrategy(const std::string& s);

/// D6 task lifecycle (F16a §3.6). Terminal states = COMPLETED / FAILED / CANCELLED.
enum class ImportTaskStatus {
    kQueued,
    kRunning,
    kCompleted,
    kFailed,
    kCancelling,
    kCancelled,
};

const char* ToString(ImportTaskStatus status);

/// One column value of a fetched row. V1.0 carries the textual rendering only
/// (the textualizer concatenates "key: value"); typed values + null distinction
/// are a Phase-2 concern. `is_null` lets PER_ROW omit / mark null columns.
struct DbValue {
    std::string text;
    bool is_null = false;
};

/// One fetched row: ordered columns + the source row identifier used to build the
/// Block `source` URI (F16a §4.3 — postgres://host/db/table/<row_id>).
struct DbRow {
    std::vector<std::pair<std::string, DbValue>> columns;  ///< preserves SELECT order
    std::string row_id;                                    ///< PK / ctid rendering for the source URI
};

/// A textualized chunk ready to feed the SPC pipeline (F16a §3.5 → §3.2). `source`
/// is the full postgres:// URI written into blocks.metadata_json (§4.3).
struct TextChunk {
    std::string text;
    std::string source;          ///< postgres://host:port/db/table/<row_id>
    nlohmann::json metadata;     ///< the §4.3 metadata_json block (source_type, ref, signature, ...)
};

/// D2 query request — oneOf table+filter (mode 1) / custom SQL (mode 2). Exactly
/// one of {table, sql} is set; the QueryExecutor validates the discriminator.
struct QueryRequest {
    // mode 1: table + filter DSL
    std::optional<std::string> table;
    nlohmann::json filter = nlohmann::json::object();   ///< v1.0.2 JSON DSL (NOT raw SQL)
    std::vector<std::string> columns;                   ///< empty = all

    // mode 2: custom SQL
    std::optional<std::string> sql;
};

/// The full import request body (POST /api/v1/import/database, F16a §6.2).
struct ImportRequest {
    NsId namespace_id;
    ConnectionRefId connection_ref;
    TextStrategy text_strategy = TextStrategy::kPerRow;
    int merge_size = 10;                                 ///< only used for kMerge
    QueryRequest query;
};

/// D6 progress snapshot (F16a §3.6 / §5.2). `error` is set only in the FAILED
/// terminal state (cancel resolves to status=cancelled, no error — §5.4 note).
struct ImportTaskProgress {
    ImportTaskId task_id;
    ImportTaskStatus status = ImportTaskStatus::kQueued;
    NsId namespace_id;
    double progress = 0.0;                               ///< 0.0 - 1.0
    int rows_imported = 0;
    int rows_total = 0;
    int rows_failed = 0;
    std::chrono::system_clock::time_point queued_at{};
    std::optional<std::chrono::system_clock::time_point> started_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;
    std::optional<std::chrono::system_clock::time_point> estimated_completion_at;
    std::optional<agent_friendly::AgentFriendlyError> error;
};

}  // namespace cortrix::import
