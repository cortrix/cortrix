#pragma once
// One pre-routing handler for the server. httplib keeps a single pre-routing
// handler: a later set_pre_routing_handler replaces the earlier one, so two
// middlewares that each install their own leave only the last one running.
// Every pre-routing stage (rate limit, AdminGuard) goes through InstallPreRouting.

#include <vector>

#include "httplib.h"
#include "cortrix/middleware/admin_guard.h"
#include "cortrix/middleware/rate_limiter.h"

namespace cortrix::middleware {

/// Install `stages` as the pre-routing handler of `server`. For each request the
/// stages run in order; the first that returns Handled ends the request, and when
/// none does the request goes on to routing. An empty stage is skipped.
void InstallPreRouting(httplib::Server& server,
                       std::vector<httplib::Server::HandlerWithResponse> stages);

/// The server binary's stages, in order: the rate limiter when `limiter` is set
/// (rate_limit.enabled), then AdminGuard. The rate limiter comes first, so
/// admin-path requests count toward the limit, the refused ones included.
std::vector<httplib::Server::HandlerWithResponse> ServerPreRoutingStages(
    RateLimiter* limiter, AdminGuardConfig admin);

}  // namespace cortrix::middleware
