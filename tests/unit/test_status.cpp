#include <gtest/gtest.h>
#include "cortrix/common/status.h"

namespace cortrix {
namespace {

TEST(StatusTest, OkStatus) {
    Status s = Status::Ok();
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kOk);
    EXPECT_EQ(s.http_status(), 200);
}

TEST(StatusTest, NotFoundStatus) {
    Status s = Status::NotFound("item not found");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_EQ(s.message(), "item not found");
    EXPECT_EQ(s.http_status(), 404);
}

TEST(StatusTest, InvalidArgumentStatus) {
    Status s = Status::InvalidArgument("bad input");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(s.message(), "bad input");
    EXPECT_EQ(s.http_status(), 400);
}

TEST(StatusTest, InternalStatus) {
    Status s = Status::Internal("server error");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_EQ(s.http_status(), 500);
}

TEST(StatusTest, UnauthenticatedStatus) {
    Status s = Status::Unauthenticated("no token");
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
    EXPECT_EQ(s.http_status(), 401);
}

TEST(StatusTest, PermissionDeniedStatus) {
    Status s = Status::PermissionDenied("no access");
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_EQ(s.http_status(), 403);
}

TEST(StatusTest, UnavailableStatus) {
    Status s = Status::Unavailable("service down");
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_EQ(s.http_status(), 503);
}

TEST(StatusTest, AlreadyExistsStatus) {
    Status s = Status::AlreadyExists("dup key");
    EXPECT_EQ(s.code(), StatusCode::kAlreadyExists);
    EXPECT_EQ(s.http_status(), 409);
}

TEST(StatusTest, ErrorCodeString) {
    EXPECT_EQ(Status::Ok().error_code_string(), "OK");
    EXPECT_EQ(Status::NotFound("").error_code_string(), "NOT_FOUND");
    EXPECT_EQ(Status::Internal("").error_code_string(), "INTERNAL_ERROR");
}

// --- Edge-case tests ---

TEST(StatusTest, DefaultConstructorIsOk) {
    Status s;
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kOk);
    EXPECT_TRUE(s.message().empty());
    EXPECT_EQ(s.http_status(), 200);
    EXPECT_EQ(s.error_code_string(), "OK");
}

TEST(StatusTest, CopySemantics) {
    Status original = Status::NotFound("missing resource");
    Status copy = original;

    EXPECT_EQ(copy.code(), StatusCode::kNotFound);
    EXPECT_EQ(copy.message(), "missing resource");
    EXPECT_EQ(copy.http_status(), 404);
}

TEST(StatusTest, MoveSemantics) {
    Status original = Status::Internal("critical failure");
    Status moved = std::move(original);

    EXPECT_EQ(moved.code(), StatusCode::kInternal);
    EXPECT_EQ(moved.message(), "critical failure");
    EXPECT_EQ(moved.http_status(), 500);
}

TEST(StatusTest, EmptyMessage) {
    Status s = Status::NotFound("");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_TRUE(s.message().empty());
    EXPECT_EQ(s.http_status(), 404);
}

TEST(StatusTest, LongMessage) {
    std::string long_msg(10000, 'x');
    Status s = Status::InvalidArgument(long_msg);
    EXPECT_EQ(s.message().size(), 10000u);
    EXPECT_EQ(s.message(), long_msg);
}

TEST(StatusTest, MessageWithSpecialCharacters) {
    Status s = Status::InvalidArgument("bad input: \"quotes\" & <tags> \n newline");
    EXPECT_EQ(s.message(), "bad input: \"quotes\" & <tags> \n newline");
}

TEST(StatusTest, AllErrorCodeStrings) {
    // Per API_GATEWAY_DESIGN.md section 2.5.2 error code definitions
    EXPECT_EQ(Status::Ok().error_code_string(), "OK");
    EXPECT_EQ(Status::InvalidArgument("").error_code_string(), "INVALID_ARGUMENT");
    EXPECT_EQ(Status::NotFound("").error_code_string(), "NOT_FOUND");
    EXPECT_EQ(Status::AlreadyExists("").error_code_string(), "CONFLICT");
    EXPECT_EQ(Status::PermissionDenied("general").error_code_string(), "FORBIDDEN");
    EXPECT_EQ(Status::Unauthenticated("Missing auth").error_code_string(), "UNAUTHORIZED");
    EXPECT_EQ(Status::Internal("").error_code_string(), "INTERNAL_ERROR");
    EXPECT_EQ(Status::Unavailable("").error_code_string(), "SERVICE_UNAVAILABLE");
}

TEST(StatusTest, AuthErrorCodeDistinction) {
    // Per API_GATEWAY_DESIGN.md section 2.5.2:
    //   INVALID_API_KEY for invalid keys
    //   EXPIRED_API_KEY for expired keys
    //   UNAUTHORIZED for missing auth header
    EXPECT_EQ(Status::Unauthenticated("Invalid API key").error_code_string(), "INVALID_API_KEY");
    EXPECT_EQ(Status::Unauthenticated("API key expired").error_code_string(), "EXPIRED_API_KEY");
    EXPECT_EQ(Status::Unauthenticated("Missing Authorization header").error_code_string(), "UNAUTHORIZED");
    EXPECT_EQ(Status::Unauthenticated("API key is empty").error_code_string(), "UNAUTHORIZED");
}

TEST(StatusTest, PermissionDeniedCodeDistinction) {
    // Per API_GATEWAY_DESIGN.md section 2.5.2:
    //   NAMESPACE_DENIED for namespace access
    //   FORBIDDEN for general permission
    EXPECT_EQ(Status::PermissionDenied("Access denied for namespace 'hr'").error_code_string(),
              "NAMESPACE_DENIED");
    EXPECT_EQ(Status::PermissionDenied("WRITE permission required").error_code_string(),
              "FORBIDDEN");
}

TEST(StatusTest, AllHttpStatusCodes) {
    EXPECT_EQ(Status::Ok().http_status(), 200);
    EXPECT_EQ(Status::InvalidArgument("").http_status(), 400);
    EXPECT_EQ(Status::Unauthenticated("").http_status(), 401);
    EXPECT_EQ(Status::PermissionDenied("").http_status(), 403);
    EXPECT_EQ(Status::NotFound("").http_status(), 404);
    EXPECT_EQ(Status::AlreadyExists("").http_status(), 409);
    EXPECT_EQ(Status::Internal("").http_status(), 500);
    EXPECT_EQ(Status::Unavailable("").http_status(), 503);
}

TEST(StatusTest, OkStatusFactoryMessage) {
    // Ok() factory should produce an empty message
    Status s = Status::Ok();
    EXPECT_TRUE(s.ok());
    EXPECT_TRUE(s.message().empty());
}

TEST(StatusTest, NonOkStatusesAreNotOk) {
    EXPECT_FALSE(Status::InvalidArgument("x").ok());
    EXPECT_FALSE(Status::NotFound("x").ok());
    EXPECT_FALSE(Status::AlreadyExists("x").ok());
    EXPECT_FALSE(Status::PermissionDenied("x").ok());
    EXPECT_FALSE(Status::Unauthenticated("x").ok());
    EXPECT_FALSE(Status::Internal("x").ok());
    EXPECT_FALSE(Status::Unavailable("x").ok());
}

TEST(StatusTest, AssignmentFromOkToError) {
    Status s = Status::Ok();
    EXPECT_TRUE(s.ok());
    s = Status::Internal("new error");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_EQ(s.message(), "new error");
}

TEST(StatusTest, AssignmentFromErrorToOk) {
    Status s = Status::NotFound("gone");
    EXPECT_FALSE(s.ok());
    s = Status::Ok();
    EXPECT_TRUE(s.ok());
    EXPECT_TRUE(s.message().empty());
}

TEST(StatusTest, MessageWithUnicodeCharacters) {
    Status s = Status::InvalidArgument("invalid: \xC3\xA9\xC3\xB1\xC3\xBC");
    EXPECT_EQ(s.message(), "invalid: \xC3\xA9\xC3\xB1\xC3\xBC");
    EXPECT_FALSE(s.ok());
}

// --- Remaining coverage: default branches and boundary cases ---

TEST(StatusTest, UnknownStatusCodeDefaultHttp) {
    // Construct a Status with an enum value outside the defined range
    // to exercise the default branch of http_status()
    Status s(static_cast<StatusCode>(99), "unknown code");
    EXPECT_EQ(s.http_status(), 500);  // default maps to 500
}

TEST(StatusTest, UnknownStatusCodeDefaultErrorString) {
    // Exercise the default branch of error_code_string()
    Status s(static_cast<StatusCode>(99), "unknown code");
    EXPECT_EQ(s.error_code_string(), "UNKNOWN");
}

TEST(StatusTest, UnauthenticatedVariantCaseSensitive) {
    // "invalid API key" (lowercase i) should still map to INVALID_API_KEY
    EXPECT_EQ(Status::Unauthenticated("invalid API key").error_code_string(), "INVALID_API_KEY");
    // "Invalid api key" (mixed case) also maps
    EXPECT_EQ(Status::Unauthenticated("Invalid api key").error_code_string(), "INVALID_API_KEY");
}

TEST(StatusTest, UnauthenticatedExpiredVariants) {
    // Both "expired" and "Expired" should map to EXPIRED_API_KEY
    EXPECT_EQ(Status::Unauthenticated("token expired").error_code_string(), "EXPIRED_API_KEY");
    EXPECT_EQ(Status::Unauthenticated("Expired key").error_code_string(), "EXPIRED_API_KEY");
}

TEST(StatusTest, PermissionDeniedNamespaceVariants) {
    // Both "namespace" and "Namespace" should map to NAMESPACE_DENIED
    EXPECT_EQ(Status::PermissionDenied("namespace access denied").error_code_string(),
              "NAMESPACE_DENIED");
    EXPECT_EQ(Status::PermissionDenied("Namespace 'x' denied").error_code_string(),
              "NAMESPACE_DENIED");
}

TEST(StatusTest, UnauthenticatedGenericMessage) {
    // A generic unauthenticated message that doesn't match "Invalid API key"
    // or "expired" should return "UNAUTHORIZED"
    EXPECT_EQ(Status::Unauthenticated("unknown reason").error_code_string(), "UNAUTHORIZED");
    EXPECT_EQ(Status::Unauthenticated("API key is empty").error_code_string(), "UNAUTHORIZED");
    EXPECT_EQ(Status::Unauthenticated("some other auth error").error_code_string(), "UNAUTHORIZED");
}

TEST(StatusTest, PermissionDeniedGenericMessage) {
    // Generic permission denied messages that don't mention "namespace"
    EXPECT_EQ(Status::PermissionDenied("READ permission required").error_code_string(),
              "FORBIDDEN");
    EXPECT_EQ(Status::PermissionDenied("").error_code_string(), "FORBIDDEN");
}

}  // namespace
}  // namespace cortrix
