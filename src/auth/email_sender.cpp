#include "cortrix/auth/email_sender.h"

#include "cortrix/logging/logging.h"

namespace cortrix::auth {

Status NullEmailSender::Send(const std::string& to, const std::string& subject,
                             const std::string& body) {
    // P08 §4.6: email_verification disabled / SMTP unconfigured → log the message
    // (so the code is recoverable in dev/CI) and succeed. Never fails.
    last_ = LastMessage{to, subject, body, /*sent=*/true};
    CORTRIX_LOG_INFO("auth.email",
                     "NullEmailSender (delivery disabled) to={} subject={} body={}",
                     to, subject, body);
    return Status::Ok();
}

}  // namespace cortrix::auth
