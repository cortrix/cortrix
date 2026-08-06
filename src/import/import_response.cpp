#include <cstdint>
#include "cortrix/import/import_response.h"

#include <ctime>

#include "cortrix/agent_friendly/error.h"

namespace cortrix::import {

std::string ToIso8601Utc(std::chrono::system_clock::time_point tp) {
    std::time_t secs = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

namespace {

// "/api/v1/import/tasks/<id>/progress" and ".../<id>" — the self-links.
std::string ProgressEndpoint(const ImportTaskId& task_id) {
    return "/api/v1/import/tasks/" + task_id + "/progress";
}
std::string CancelEndpoint(const ImportTaskId& task_id) {
    return "/api/v1/import/tasks/" + task_id;
}

}  // namespace

nlohmann::json BuildImportStartedResponse(const ImportTaskProgress& task,
                                          const ConnectionRefId& connection_ref,
                                          int64_t estimated_rows) {
    nlohmann::json body;
    body["task_id"] = task.task_id;
    //: the POST /import/database response is the ACCEPTANCE ack — its status is
    // deterministically "queued" (enqueued, returned immediately). We do NOT echo
    // task.status here: a worker thread may already have advanced it to "running"
    // (or further) by the time the handler builds this body, which would race the
    // async pipeline and break the stable schema. Live status is the GET /progress
    // endpoint's job (BuildProgressResponse), not the acceptance ack's.
    body["status"] = ToString(ImportTaskStatus::kQueued);
    body["namespace"] = task.namespace_id;
    body["connection_ref"] = connection_ref;
    body["estimated_rows"] = estimated_rows;
    body["queued_at"] = ToIso8601Utc(task.queued_at);
    body["progress_endpoint"] = ProgressEndpoint(task.task_id);
    body["cancel_endpoint"] = CancelEndpoint(task.task_id);
    body["error"] = nullptr;  // success body always carries explicit null (GEN-Agent stable schema)
    return body;
}

nlohmann::json BuildProgressResponse(const ImportTaskProgress& task) {
    nlohmann::json body;
    body["task_id"] = task.task_id;
    body["status"] = ToString(task.status);
    body["progress"] = task.progress;
    body["rows_imported"] = task.rows_imported;
    body["rows_total"] = task.rows_total;
    body["rows_failed"] = task.rows_failed;
    body["started_at"] =
        task.started_at ? nlohmann::json(ToIso8601Utc(*task.started_at)) : nlohmann::json(nullptr);
    body["estimated_completion_at"] =
        task.estimated_completion_at
            ? nlohmann::json(ToIso8601Utc(*task.estimated_completion_at))
            : nlohmann::json(nullptr);
    // note: cancel is not an error — only the FAILED terminal state carries a body.
    body["error"] = task.error ? agent_friendly::ToJson(*task.error) : nlohmann::json(nullptr);
    return body;
}

}  // namespace cortrix::import
