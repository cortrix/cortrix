// Error-path coverage for the Secret CX_ERR_* codes.
//
// CX_ERR_SECRET_NOT_FOUND      <-> SecretError::kNotFound
// CX_ERR_SECRET_INVALID_CONFIG <-> SecretError::kInvalidConfig
//
// IMPORTANT (and reported as DEFERRED): in Phase 1 NO production code path
// returns either SecretError. The only shipped provider, EnvSecretProvider,
// answers an absent env var with Ok(nullopt) -- never Err(kNotFound) -- and never
// emits kInvalidConfig. The CX_ERR_SECRET_* spellings exist only in design
// comments; they are not wired to any throw/return site. So there is no real
// precondition to drive at the provider object layer for these two identities.
//
// What IS genuinely testable -- and previously had zero coverage -- is the
// SecretResult::Err(...) error-carrying contract itself: the single mechanism by
// which a (future Cloud) provider surfaces these codes. We pin that contract here
// so the Phase-2 provider that finally emits kNotFound / kInvalidConfig drops in
// against an asserted shape. This is the error path of SecretResult, exercised
// for the exact two enum values that map to the assigned CX codes -- not a string
// reference to the codes.

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "cortrix/security/i_secret_provider.h"

namespace cortrix::security {
namespace {

// SecretResult::Err(kNotFound): not ok, carries kNotFound, no value. This is the
// shape a vault provider returns for "key absent in the vault" (distinct from
// EnvSecretProvider's Ok(nullopt) "unset" answer). ToString pins the wire token.
TEST(SecretErrPathTest, ErrNotFoundCarriesNotFoundAndNoValue) {
    SecretResult r = SecretResult::Err(SecretError::kNotFound);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error(), SecretError::kNotFound);
    EXPECT_FALSE(r.value().has_value());
    EXPECT_STREQ(ToString(r.error()), "not_found");
}

// SecretResult::Err(kInvalidConfig): the provider-misconfigured error path.
TEST(SecretErrPathTest, ErrInvalidConfigCarriesInvalidConfigAndNoValue) {
    SecretResult r = SecretResult::Err(SecretError::kInvalidConfig);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error(), SecretError::kInvalidConfig);
    EXPECT_FALSE(r.value().has_value());
    EXPECT_STREQ(ToString(r.error()), "invalid_config");
}

// The two error identities must stay distinct (a Cloud provider routes on them).
TEST(SecretErrPathTest, NotFoundAndInvalidConfigAreDistinct) {
    EXPECT_NE(SecretError::kNotFound, SecretError::kInvalidConfig);
}

}  // namespace
}  // namespace cortrix::security
