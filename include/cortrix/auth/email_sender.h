#pragma once
#include <string>

#include "cortrix/common/status.h"

namespace cortrix::auth {

/// Pluggable email transport (P08 §4.6). Phase 1 default is NullEmailSender
/// (email_verification off → codes only logged); SmtpEmailSender is the
/// production transport, configured via the admin API (topic 8).
class IEmailSender {
public:
    virtual ~IEmailSender() = default;
    virtual Status Send(const std::string& to,
                        const std::string& subject,
                        const std::string& body) = 0;
};

/// Default sender (P08 §4.6): does not send; logs the message at INFO so a dev /
/// CI run can read the verification code. Used whenever email_verification is
/// false (the §3.6 default) or SMTP is unconfigured. Always succeeds.
class NullEmailSender : public IEmailSender {
public:
    Status Send(const std::string& to, const std::string& subject,
                const std::string& body) override;

    /// Test hook: the last (to, subject, body) passed to Send (so a unit test can
    /// assert the verification code reached the sender without real delivery).
    struct LastMessage { std::string to, subject, body; bool sent = false; };
    const LastMessage& last() const { return last_; }

private:
    LastMessage last_;
};

}  // namespace cortrix::auth
