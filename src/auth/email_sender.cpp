#include "cortrix/auth/email_sender.h"

#include "cortrix/logging/logging.h"
#include "cortrix/logging/sanitizer.h"

namespace cortrix::auth {

Status NullEmailSender::Send(const std::string& to, const std::string& subject,
                             const std::string& body) {
    // email_verification disabled / SMTP unconfigured → log the message
    // (so the code is recoverable in dev/CI) and succeed. Never fails.
    last_ = LastMessage{to, subject, body, /*sent=*/true};
    // The recipient is PII (email) → mask via LogSanitizer before it hits a sink.
    // subject/body stay verbatim: this NullSender only runs when delivery is
    // disabled (dev/CI), where the body must remain recoverable to read the code.
    CORTRIX_LOG_INFO("auth.email",
                     "NullEmailSender (delivery disabled) to={} subject={} body={}",
                     cortrix::logging::SanitizeForLog("email", to), subject, body);
    return Status::Ok();
}

}  // namespace cortrix::auth
