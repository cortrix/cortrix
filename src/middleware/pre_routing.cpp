#include "cortrix/middleware/pre_routing.h"

#include <utility>
#include <vector>

namespace cortrix::middleware {

void InstallPreRouting(httplib::Server& server,
                       std::vector<httplib::Server::HandlerWithResponse> stages) {
    server.set_pre_routing_handler(
        [stages = std::move(stages)](const httplib::Request& req, httplib::Response& res) {
            for (const auto& stage : stages) {
                if (stage && stage(req, res) == httplib::Server::HandlerResponse::Handled) {
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
