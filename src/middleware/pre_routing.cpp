#include "cortrix/middleware/pre_routing.h"

#include <utility>
#include <vector>

#include "cortrix/server/http_server.h"
#include "cortrix/server/request_context.h"

namespace cortrix::middleware {

void InstallPreRouting(httplib::Server& server,
                       std::vector<httplib::Server::HandlerWithResponse> stages) {
    server.set_pre_routing_handler(
        [stages = std::move(stages)](const httplib::Request& req, httplib::Response& res) {
            for (const auto& stage : stages) {
                if (stage && stage(req, res) == httplib::Server::HandlerResponse::Handled) {
                    // A stage that answers (rate limit 429, admin loopback 403) runs
                    // before any route wrapper mints a request id, so mint one here.
                    CompleteErrorEnvelope(res, GenerateRequestId());
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
}

std::vector<httplib::Server::HandlerWithResponse> ServerPreRoutingStages(
    RateLimiter* limiter, AdminGuardConfig admin) {
    std::vector<httplib::Server::HandlerWithResponse> stages;
    if (limiter != nullptr) stages.push_back(RateLimitHandler(*limiter));
    stages.push_back(AdminGuardHandler(std::move(admin)));
    return stages;
}

}  // namespace cortrix::middleware
