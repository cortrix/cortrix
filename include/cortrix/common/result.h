#pragma once
#include <optional>
#include <utility>

#include "cortrix/common/status.h"

namespace cortrix {

/// StatusOr-style result: holds either a value (Ok) or an error Status.
///
/// Convention (the coding conventions): a fallible operation that returns a
/// value returns `Result<T>`; a fallible operation that returns nothing returns
/// plain `Status`. An Ok future/Result always carries a value; an error Result
/// carries only the Status and value() must not be called.
template <typename T>
class Result {
public:
    // Success: wrap a value.
    Result(T value) : status_(Status::Ok()), value_(std::move(value)) {}

    // Failure: wrap an error Status (must not be Ok — that is a misuse).
    Result(Status error) : status_(std::move(error)) {}

    bool ok() const { return status_.ok(); }
    const Status& status() const { return status_; }

    // Value accessors — only valid when ok().
    T& value() { return *value_; }
    const T& value() const { return *value_; }
    T& operator*() { return *value_; }
    const T& operator*() const { return *value_; }
    T* operator->() { return &*value_; }
    const T* operator->() const { return &*value_; }

private:
    Status status_;
    std::optional<T> value_;
};

}  // namespace cortrix
