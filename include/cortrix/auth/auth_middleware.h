#pragma once
#include <functional>
#include <string>
#include "httplib.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/server/request_context.h"

namespace cortrix {

/// HTTP handler with request context
using HttpHandler = std::function<void(const httplib::Request&, httplib::Response&,
                                        const RequestContext&)>;

/// Auth middleware: authenticate + authorize, then call handler
httplib::Server::Handler WithAuth(
    ApiKeyAuth& auth,
    int required_permission,
    HttpHandler handler
);

/// Same, and when the key lacks `required_permission` the 403 message carries
/// `denied_hint` after the standard "<PERM> permission required", telling the
/// caller what to do next.
httplib::Server::Handler WithAuth(
    ApiKeyAuth& auth,
    int required_permission,
    HttpHandler handler,
    std::string denied_hint
);

/// No-auth handler wrapper (for /health etc.)
httplib::Server::Handler NoAuth(HttpHandler handler);

}  // namespace cortrix
