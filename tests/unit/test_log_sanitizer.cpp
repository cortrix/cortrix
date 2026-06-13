#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/logging/sanitizer.h"

namespace cortrix::logging {
namespace {

using nlohmann::json;

// Mirror of src/logging/sensitive_fields.yaml so the test is hermetic (does not
// depend on a file on disk). If the rule file changes, this should track it.
const char* kRules = R"(
P0_redact_completely:
  - api_key
  - access_token
  - bearer_token
  - jwt_secret
  - password
  - openai_api_key
  - smtp_password
  - license_key
  - admin_token
  - connection_string
  - master_key
P0_mask_partial:
  - api_key_prefix
P1_redact_pii:
  - email
  - phone
  - ip_address
  - real_name
  - postal_code
P1_truncate_content:
  - query_text
  - document_content
  - memory_text
  - prompt
P2_hash_only:
  - doc_id
  - user_id
safe_fields:
  - request_id
  - trace_id
)";

LogSanitizer MakeSanitizer() { return LogSanitizer(kRules); }

// ---- P0 full redaction (11 fields) ----

TEST(LogSanitizerTest, P0FieldsFullyRedacted) {
    auto s = MakeSanitizer();
    const char* p0[] = {"api_key", "access_token", "bearer_token", "jwt_secret",
                        "password", "openai_api_key", "smtp_password",
                        "license_key", "admin_token", "connection_string",
                        "master_key"};
    for (const char* field : p0) {
        EXPECT_EQ(s.SanitizeField(field, "super-secret-value"), "****")
            << "field=" << field;
    }
}

// ---- P0 partial mask (1 field) ----

TEST(LogSanitizerTest, ApiKeyPrefixPartialMask) {
    auto s = MakeSanitizer();
    // 8-char head + *** + 3-char tail.
    EXPECT_EQ(s.SanitizeField("api_key_prefix", "sk-prod-abcdefghxyz"),
              "sk-prod-***xyz");
}

TEST(LogSanitizerTest, ApiKeyPrefixTooShortFullyMasked) {
    auto s = MakeSanitizer();
    EXPECT_EQ(s.SanitizeField("api_key_prefix", "short"), "***");
}

// ---- P1 PII (5 fields) ----

TEST(LogSanitizerTest, EmailMasked) {
    auto s = MakeSanitizer();
    EXPECT_EQ(s.SanitizeField("email", "user@example.com"), "****@***");
}

TEST(LogSanitizerTest, IpAddressKeepsFirstTwoOctets) {
    auto s = MakeSanitizer();
    EXPECT_EQ(s.SanitizeField("ip_address", "192.168.1.42"), "192.168.***.***");
}

TEST(LogSanitizerTest, OtherPiiFullyMasked) {
    auto s = MakeSanitizer();
    EXPECT_EQ(s.SanitizeField("phone", "+1-555-0100"), "****");
    EXPECT_EQ(s.SanitizeField("real_name", "Jane Doe"), "****");
    EXPECT_EQ(s.SanitizeField("postal_code", "94103"), "****");
}

// ---- P1 content truncation (4 fields) ----

TEST(LogSanitizerTest, QueryTextPreviewPlusHash) {
    auto s = MakeSanitizer();
    std::string long_query(120, 'a');
    std::string out = s.SanitizeField("query_text", long_query);
    // First 50 chars preview + sha256 hash marker.
    EXPECT_EQ(out.substr(0, 50), std::string(50, 'a'));
    EXPECT_NE(out.find("...[sha256:"), std::string::npos);
}

TEST(LogSanitizerTest, ContentFieldsHashOnly) {
    auto s = MakeSanitizer();
    for (const char* field : {"document_content", "memory_text", "prompt"}) {
        std::string out = s.SanitizeField(field, "the quick brown fox");
        EXPECT_EQ(out.rfind("[sha256:", 0), 0u) << "field=" << field;
    }
}

// ---- P2 hash-only (2 fields) ----

TEST(LogSanitizerTest, CorrelationKeysHashed) {
    auto s = MakeSanitizer();
    std::string doc = s.SanitizeField("doc_id", "doc-123");
    std::string usr = s.SanitizeField("user_id", "user-456");
    EXPECT_EQ(doc.rfind("[sha256:", 0), 0u);
    EXPECT_EQ(usr.rfind("[sha256:", 0), 0u);
    // Deterministic: same input -> same hash.
    EXPECT_EQ(doc, s.SanitizeField("doc_id", "doc-123"));
    // Different input -> different hash.
    EXPECT_NE(doc, s.SanitizeField("doc_id", "doc-999"));
}

// ---- pass-through + control checks ----

TEST(LogSanitizerTest, SafeAndUnknownFieldsUnchanged) {
    auto s = MakeSanitizer();
    EXPECT_EQ(s.SanitizeField("request_id", "req-abc"), "req-abc");
    EXPECT_EQ(s.SanitizeField("status_code", "200"), "200");
    EXPECT_FALSE(s.IsControlled("request_id"));
    EXPECT_TRUE(s.IsControlled("api_key"));
}

// ---- recursive JSON sanitization ----

TEST(LogSanitizerTest, SanitizeNestedObject) {
    auto s = MakeSanitizer();
    json payload = {
        {"request_id", "req-1"},
        {"api_key", "sk-live-xyz"},
        {"user", {{"email", "a@b.com"}, {"user_id", "u-1"}, {"name_label", "ok"}}},
    };
    json out = s.Sanitize(payload);
    EXPECT_EQ(out["request_id"], "req-1");           // safe, untouched
    EXPECT_EQ(out["api_key"], "****");               // redacted
    EXPECT_EQ(out["user"]["email"], "****@***");     // nested PII masked
    EXPECT_EQ(out["user"]["user_id"].get<std::string>().rfind("[sha256:", 0), 0u);
    EXPECT_EQ(out["user"]["name_label"], "ok");      // nested unknown, untouched
}

TEST(LogSanitizerTest, SanitizeArrayOfObjects) {
    auto s = MakeSanitizer();
    json payload = {{"items", json::array({
        {{"doc_id", "d1"}, {"trace_id", "t1"}},
        {{"doc_id", "d2"}, {"password", "pw"}},
    })}};
    json out = s.Sanitize(payload);
    EXPECT_EQ(out["items"][0]["doc_id"].get<std::string>().rfind("[sha256:", 0), 0u);
    EXPECT_EQ(out["items"][0]["trace_id"], "t1");
    EXPECT_EQ(out["items"][1]["password"], "****");
}

TEST(LogSanitizerTest, EmptyRulesPassThrough) {
    LogSanitizer s("");  // no rules loaded
    json payload = {{"api_key", "secret"}, {"email", "a@b.com"}};
    json out = s.Sanitize(payload);
    // Nothing controlled -> everything passes through.
    EXPECT_EQ(out["api_key"], "secret");
    EXPECT_EQ(out["email"], "a@b.com");
    EXPECT_FALSE(s.IsControlled("api_key"));
}

// ---- branch coverage: YAML loading edge cases (LoadSet, ctor catch) ----

// Malformed YAML -> constructor catch path; nothing is controlled afterwards.
TEST(LogSanitizerTest, MalformedYamlYieldsPassThrough) {
    LogSanitizer s("P0_redact_completely: [unterminated");
    EXPECT_FALSE(s.IsControlled("api_key"));
    EXPECT_EQ(s.SanitizeField("api_key", "secret"), "secret");
}

// A controlled key present but not a sequence (scalar) -> LoadSet skips it.
TEST(LogSanitizerTest, NonSequenceRuleSectionIgnored) {
    LogSanitizer s("P0_redact_completely: just_a_scalar");
    EXPECT_FALSE(s.IsControlled("api_key"));
}

// A rule section that is entirely absent -> LoadSet early-returns on missing key.
TEST(LogSanitizerTest, MissingRuleSectionIgnored) {
    // Only P2 present; P0/P1 keys are absent from the document.
    LogSanitizer s("P2_hash_only:\n  - doc_id\n");
    EXPECT_TRUE(s.IsControlled("doc_id"));
    EXPECT_FALSE(s.IsControlled("api_key"));   // P0 section absent
    EXPECT_FALSE(s.IsControlled("email"));     // P1 section absent
}

// ---- branch coverage: MaskPii IPv4 fallbacks ----

// IP value with a single dot (no second octet separator) -> fully masked "***".
TEST(LogSanitizerTest, IpAddressSingleDotFullyMasked) {
    auto s = MakeSanitizer();
    EXPECT_EQ(s.SanitizeField("ip_address", "10.fragment"), "***");
}

// IP value with no dots at all (e.g. IPv6) -> fully masked "***".
TEST(LogSanitizerTest, IpAddressNoDotFullyMasked) {
    auto s = MakeSanitizer();
    EXPECT_EQ(s.SanitizeField("ip_address", "fe80::1"), "***");
}

// ---- branch coverage: TruncateContent short query_text (no truncation) ----

TEST(LogSanitizerTest, QueryTextShorterThanPreviewKeptWhole) {
    auto s = MakeSanitizer();
    std::string short_query = "hi";  // < 50 chars -> preview == value
    std::string out = s.SanitizeField("query_text", short_query);
    EXPECT_EQ(out.rfind("hi...[sha256:", 0), 0u);
}

// ---- branch coverage: SanitizeField P2 fall-through ----

// Exercises the p2_hash_ branch of SanitizeField directly (vs. via P0/P1).
TEST(LogSanitizerTest, SanitizeFieldP2HashBranch) {
    auto s = MakeSanitizer();
    std::string out = s.SanitizeField("user_id", "u-1");
    EXPECT_EQ(out.rfind("[sha256:", 0), 0u);
}

// ---- branch coverage: SanitizeValue non-string controlled scalar (value.dump) ----

// A controlled key whose JSON value is a number -> coerced via dump() then hashed.
TEST(LogSanitizerTest, ControlledNumericScalarCoercedThenHashed) {
    auto s = MakeSanitizer();
    json payload = {{"doc_id", 12345}};  // number, not string
    json out = s.Sanitize(payload);
    EXPECT_EQ(out["doc_id"].get<std::string>().rfind("[sha256:", 0), 0u);
}

// A controlled key whose JSON value is a bool -> coerced via dump() then redacted.
TEST(LogSanitizerTest, ControlledBoolScalarCoerced) {
    auto s = MakeSanitizer();
    json payload = {{"password", true}};
    json out = s.Sanitize(payload);
    EXPECT_EQ(out["password"], "****");
}

// ---- branch coverage: Sanitize array of scalars + top-level non-object ----

// Array containing plain scalars: elements are passed through unchanged.
TEST(LogSanitizerTest, ArrayOfScalarsPassThrough) {
    auto s = MakeSanitizer();
    json payload = {{"tags", json::array({"a", "b", 3})}};
    json out = s.Sanitize(payload);
    EXPECT_EQ(out["tags"][0], "a");
    EXPECT_EQ(out["tags"][1], "b");
    EXPECT_EQ(out["tags"][2], 3);
}

// A nested array directly under a controlled-looking key still recurses (array branch).
TEST(LogSanitizerTest, NestedArrayUnderKeyRecurses) {
    auto s = MakeSanitizer();
    json payload = {{"records", json::array({
        json::array({{{"api_key", "sk-1"}}}),
    })}};
    json out = s.Sanitize(payload);
    EXPECT_EQ(out["records"][0][0]["api_key"], "****");
}

// Top-level scalar payload (no key to match) -> returned unchanged.
TEST(LogSanitizerTest, TopLevelScalarUnchanged) {
    auto s = MakeSanitizer();
    json scalar = "just a string";
    EXPECT_EQ(s.Sanitize(scalar), scalar);
    json number = 42;
    EXPECT_EQ(s.Sanitize(number), number);
}

// Top-level array payload (no key) -> walked, nested objects sanitized.
TEST(LogSanitizerTest, TopLevelArrayWalked) {
    auto s = MakeSanitizer();
    json arr = json::array({{{"password", "pw"}}, "scalar"});
    json out = s.Sanitize(arr);
    EXPECT_EQ(out[0]["password"], "****");
    EXPECT_EQ(out[1], "scalar");
}

// ---- branch coverage: Initialize file-not-found path + Global before init ----

// Initialize with a path that does not exist -> installs empty pass-through.
TEST(LogSanitizerTest, InitializeMissingFileInstallsPassThrough) {
    LogSanitizer::Initialize("/nonexistent/path/sensitive_fields.yaml");
    const LogSanitizer* g = LogSanitizer::Global();
    ASSERT_NE(g, nullptr);
    EXPECT_FALSE(g->IsControlled("api_key"));  // empty rules -> nothing controlled
}

}  // namespace
}  // namespace cortrix::logging
