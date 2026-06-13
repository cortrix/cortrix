#include "cortrix/query/query_response.h"
#include "cortrix/query/scored_block.h"

namespace cortrix {

json QueryResponse::ToJson() const {
    json j;

    // results array
    json results_arr = json::array();
    for (const auto& item : results) {
        json r;
        r["block_id"] = item.block_id;
        r["doc_id"] = item.doc_id;
        r["score"] = item.score;
        r["source_path"] = item.source_path;
        r["block_type"] = item.block_type;
        r["chunk_text"] = item.chunk_text;
        r["metadata"] = item.metadata;
        r["related_blocks_count"] = item.related_blocks_count;

        // Per-route hit info and raw scores
        json hit_arr = json::array();
        if (item.hit_routes & kRouteVector) hit_arr.push_back("vector");
        if (item.hit_routes & kRouteBM25)   hit_arr.push_back("bm25");
        if (item.hit_routes & kRouteSQL)    hit_arr.push_back("sql");
        r["hit_routes"] = std::move(hit_arr);
        if (item.hit_routes & kRouteVector) r["vector_score"] = item.vector_score;
        if (item.hit_routes & kRouteBM25)   r["bm25_score"] = item.bm25_score;

        results_arr.push_back(std::move(r));
    }
    j["results"] = std::move(results_arr);

    // sql_result
    if (sql_result.has_result) {
        json sql;
        sql["query"] = sql_result.query;
        sql["columns"] = sql_result.columns;
        json rows_arr = json::array();
        for (const auto& row : sql_result.rows) {
            rows_arr.push_back(row);
        }
        sql["rows"] = std::move(rows_arr);
        sql["row_count"] = sql_result.row_count;
        sql["truncated"] = sql_result.truncated;
        if (!sql_result.error.empty()) {
            sql["error"] = sql_result.error;
        }
        sql["sql_latency_ms"] = sql_result.sql_latency_ms;
        j["sql_result"] = std::move(sql);
    } else {
        j["sql_result"] = nullptr;
    }

    // meta
    json m;
    m["total_results"] = meta.total_results;
    m["latency_ms"] = meta.latency_ms;
    m["routes_used"] = meta.routes_used;
    m["degraded"] = meta.degraded;
    m["degraded_routes"] = meta.degraded_routes;
    m["degradation_level"] = meta.degradation_level;
    j["meta"] = std::move(m);

    // error (only present when has_error is true)
    if (has_error) {
        j["error"] = error_message;
    }

    return j;
}

}  // namespace cortrix
