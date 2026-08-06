#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/batch_builder.h"
#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_error.h"

// S5.1 coverage (catalog Issue 5): structured_data required-keys per
// error code + the BatchResult/BatchMeta builder (5 meta fields + coverage_ratio).
namespace cortrix::catalog {
namespace {

const std::vector<CatalogErrorCode>& AllCodes() {
    static const std::vector<CatalogErrorCode> codes = {
        CatalogErrorCode::kNsNotFound, CatalogErrorCode::kNsUnauthorized,
        CatalogErrorCode::kNsIsolated, CatalogErrorCode::kNsVisibilityDenied,
        CatalogErrorCode::kNsAlreadyExists, CatalogErrorCode::kUnitNotFound,
        CatalogErrorCode::kUnitNotSealed, CatalogErrorCode::kUnitArchived,
        CatalogErrorCode::kUnitOwnerRemote, CatalogErrorCode::kTenantNotFound,
        CatalogErrorCode::kTenantQuotaExceeded, CatalogErrorCode::kSchemaVersionMismatch,
        CatalogErrorCode::kSchemaMigrationFailed, CatalogErrorCode::kInvalidConfigJson,
        CatalogErrorCode::kCatalogLocked, CatalogErrorCode::kBloomFilterNotReady,
        CatalogErrorCode::kTransactionRetry, CatalogErrorCode::kContentHashMismatch,
        CatalogErrorCode::kRefCountNegative, CatalogErrorCode::kInternalError,
        CatalogErrorCode::kNotImplemented, CatalogErrorCode::kNsQuotaExceeded,
        CatalogErrorCode::kNsResourceBudgetExceeded, CatalogErrorCode::kNsPoolInternal,
    };
    return codes;
}

// test #1: every code (except the case-by-case INTERNAL_ERROR) declares at
// least one required structured_data key; INTERNAL_ERROR declares none.
TEST(BatchResponseTest, EveryCodeDeclaresRequiredKeys) {
    for (CatalogErrorCode code : AllCodes()) {
        const auto& keys = RequiredStructuredDataKeys(code);
        if (code == CatalogErrorCode::kInternalError) {
            EXPECT_TRUE(keys.empty()) << "INTERNAL_ERROR is case-by-case";
        } else {
            EXPECT_FALSE(keys.empty())
                << CatalogErrorCodeString(code) << " must declare required keys";
        }
    }
}

// HasRequiredStructuredData enforces presence of every required key.
TEST(BatchResponseTest, ValidatorRejectsMissingKeysAcceptsComplete) {
    // NS_NOT_FOUND requires {ns_id}.
    EXPECT_FALSE(HasRequiredStructuredData(
        CatalogErrorCode::kNsNotFound, nlohmann::json::object()));
    EXPECT_TRUE(HasRequiredStructuredData(
        CatalogErrorCode::kNsNotFound, {{"ns_id", "ns-1"}}));

    // NS_UNAUTHORIZED requires 4 keys — a partial set fails.
    nlohmann::json partial = {{"ns_id", "n"}, {"tenant_id", "t"}};
    EXPECT_FALSE(HasRequiredStructuredData(CatalogErrorCode::kNsUnauthorized, partial));
    nlohmann::json full = {{"ns_id", "n"}, {"tenant_id", "t"},
                           {"user_id", "u"}, {"required_permission", "read"}};
    EXPECT_TRUE(HasRequiredStructuredData(CatalogErrorCode::kNsUnauthorized, full));
}

// INTERNAL_ERROR (no required keys) is satisfied by any payload incl. empty.
TEST(BatchResponseTest, InternalErrorHasNoRequiredKeys) {
    EXPECT_TRUE(HasRequiredStructuredData(
        CatalogErrorCode::kInternalError, nlohmann::json::object()));
    EXPECT_TRUE(HasRequiredStructuredData(
        CatalogErrorCode::kInternalError, {{"anything", 1}}));
}

// A MakeCatalogError body built with the required keys validates clean — the
// end-to-end Agent-friendly contract (build → validate).
TEST(BatchResponseTest, MakeCatalogErrorWithRequiredKeysValidates) {
    auto err = MakeCatalogError(CatalogErrorCode::kRefCountNegative,
                                {{"file_hash", "fh"}, {"ns_id", "ns"}, {"current_ref_count", 0}});
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_TRUE(HasRequiredStructuredData(CatalogErrorCode::kRefCountNegative,
                                          *err.structured_data));
}

// test #3: coverage_ratio = succeeded / (succeeded + failed).
TEST(BatchResponseTest, BuilderComputesCoverageRatio) {
    BatchContext ctx;
    ctx.catalog_version = 7;
    ctx.bloom_filter_ready = true;
    ctx.timestamp_ms = 123;
    ctx.node_id = "local";

    BatchBuilder<std::string> b(ctx);
    b.AddSuccess("a", "A");
    b.AddSuccess("b", "B");
    b.AddSuccess("c", "C");
    b.AddFailure("d", "CX_ERR_NS_NOT_FOUND", "missing");
    BatchResult<std::string> r = b.Build();

    EXPECT_EQ(r.results.size(), 3u);
    EXPECT_EQ(r.meta.succeeded_ids.size(), 3u);
    ASSERT_EQ(r.meta.failed.size(), 1u);
    EXPECT_EQ(r.meta.failed[0].cx_code, "CX_ERR_NS_NOT_FOUND");
    EXPECT_FLOAT_EQ(r.meta.coverage_ratio, 3.0f / 4.0f);  // 0.75
    // 5 standard meta fields stamped + api_version "v1".
    EXPECT_EQ(r.meta.catalog_version, 7);
    EXPECT_TRUE(r.meta.bloom_filter_ready);
    EXPECT_EQ(r.meta.timestamp_ms, 123);
    EXPECT_EQ(r.meta.node_id, "local");
    EXPECT_EQ(r.meta.api_version, "v1");
}

TEST(BatchResponseTest, EmptyBatchIsFullyCovered) {
    BatchBuilder<std::string> b(BatchContext{});
    BatchResult<std::string> r = b.Build();
    EXPECT_TRUE(r.results.empty());
    EXPECT_FLOAT_EQ(r.meta.coverage_ratio, 1.0f);  // convention: 0/0 → 1.0
}

TEST(BatchResponseTest, AllFailedIsZeroCoverage) {
    BatchBuilder<std::string> b(BatchContext{});
    b.AddFailure("x", "CX_ERR_NS_NOT_FOUND");
    b.AddFailure("y", "CX_ERR_NS_NOT_FOUND");
    BatchResult<std::string> r = b.Build();
    EXPECT_TRUE(r.results.empty());
    EXPECT_FLOAT_EQ(r.meta.coverage_ratio, 0.0f);
}

}  // namespace
}  // namespace cortrix::catalog
