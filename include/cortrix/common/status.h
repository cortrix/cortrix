#pragma once
#include <string>

namespace cortrix {

enum class StatusCode {
    kOk = 0,
    kInvalidArgument = 1,
    kNotFound = 2,
    kAlreadyExists = 3,
    kPermissionDenied = 4,
    kUnauthenticated = 5,
    kInternal = 6,
    kUnavailable = 7,
};

class Status {
public:
    Status() : code_(StatusCode::kOk) {}
    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    static Status Ok() { return Status(); }
    static Status InvalidArgument(const std::string& msg) {
        return Status(StatusCode::kInvalidArgument, msg);
    }
    static Status NotFound(const std::string& msg) {
        return Status(StatusCode::kNotFound, msg);
    }
    static Status AlreadyExists(const std::string& msg) {
        return Status(StatusCode::kAlreadyExists, msg);
    }
    static Status PermissionDenied(const std::string& msg) {
        return Status(StatusCode::kPermissionDenied, msg);
    }
    static Status Unauthenticated(const std::string& msg) {
        return Status(StatusCode::kUnauthenticated, msg);
    }
    static Status Internal(const std::string& msg) {
        return Status(StatusCode::kInternal, msg);
    }
    static Status Unavailable(const std::string& msg) {
        return Status(StatusCode::kUnavailable, msg);
    }

    bool ok() const { return code_ == StatusCode::kOk; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }

    /// StatusCode -> HTTP status code mapping
    int http_status() const;

    /// StatusCode -> JSON error code string (e.g. "NOT_FOUND")
    std::string error_code_string() const;

private:
    StatusCode code_;
    std::string message_;
};

}  // namespace cortrix
